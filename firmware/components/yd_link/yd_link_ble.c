/*
 * yd_link の BLE トランスポート（NimBLE）
 *
 *  机上ユニット = Peripheral（GATTサーバー）
 *      Service  YD_SVC
 *        ├ TO_DESK  : Write Without Response（暗号化必須）  レシーバー → 机上
 *        └ TO_RECV  : Notify                                 机上 → レシーバー
 *  レシーバー = Central（GATTクライアント）
 *      YD_SVC をアドバタイズしている机上ユニットを探して接続 → ペアリング/暗号化
 *      → サービス探索 → TO_RECV の通知を購読 → リンク確立(up)
 *
 *  1パケット = [ch(1) | payload]。BLEはリンク層で順序保証・再送されるのでCRCは不要。
 *
 *  セキュリティ:
 *   - LE Secure Connections + ボンディング（鍵はNVSに保存）
 *   - データの送受信は暗号化済みリンクのみ
 *   - 机上ユニットは「まだ誰ともペアリングしていない」か「起動後 PAIRING_WINDOW_S 秒以内」
 *     だけ新規ペアリングを受け付ける。それ以外は登録済みの相手以外を切断する
 *   - レシーバーは最初にペアリングした机上ユニットのアドレスを覚え、以後それにしか繋がない
 *   - 入出力のない機器同士なので BLE のペアリング自体は Just Works（中間者攻撃に弱い）
 *   - その上に yd_sec（事前共有鍵（鍵ファイル keyrelay-key.bin）による相互認証＋AES-256-GCM）を重ねる。
 *     鍵を知らない相手とはペアリングできても1バイトもやり取りしない（yd_sec.h 参照）
 *
 *  状態: s_ready = BLE として送受信できる（暗号化＋購読済み）
 *        s_up    = yd_sec のハンドシェイク完了（ここで初めてアプリに「接続」を知らせる）
 */
#include <string.h>
#include "yd_link_priv.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <inttypes.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_partition.h"
#include "yd_sec.h"
#include "yd_bond_policy.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "yd_ble";

void ble_store_config_init(void);

#define PAIRING_WINDOW_S   120
#define BLE_PKT_MAX        244          /* 1(ch) + 243 */
#define TXQ_DEPTH          48
#define RXQ_DEPTH          48
#define RXQ_EV_RESERVE     4            /* 受信データで埋めず、接続・切断の通知用に空けておく枠 */
#define DEVICE_NAME        "KeyRelay"

/* 128bit UUID（リトルエンディアン）: 7d1e0000-5c4a-4f6e-9b2d-59d0c0ffee01 系 */
#define YD_UUID(last) BLE_UUID128_INIT(0x01, 0xee, 0xff, 0xc0, 0xd0, 0x59, 0x2d, 0x9b, \
                                       0x6e, 0x4f, 0x4a, 0x5c, last, 0x00, 0x1e, 0x7d)
static const ble_uuid128_t UUID_SVC     = YD_UUID(0x00);
static const ble_uuid128_t UUID_TO_DESK = YD_UUID(0x01);
static const ble_uuid128_t UUID_TO_RECV = YD_UUID(0x02);

#define HS_TIMEOUT_MS      5000          /* この時間内に相互認証が終わらなければ切断 */
#define KEY_PARTITION      "ydkey"       /* 鍵を書き込むパーティション（partitions.csv） */

typedef enum {
    PK_DATA = 0,        /* rx: 相手から届いたパケット / tx: アプリのデータ（暗号化して送る） */
    PK_RAW,             /* tx: ハンドシェイク（そのまま送る） */
    PK_EV_READY,        /* rx: BLE として送受信できるようになった（ハンドシェイク開始） */
    PK_EV_DOWN,         /* rx: 切れた */
} pk_kind_t;

typedef struct {
    uint8_t kind;
    uint16_t len;
    uint8_t buf[BLE_PKT_MAX];
} pkt_t;

static yd_link_config_t s_cfg;
static bool s_is_central;
static uint8_t s_own_addr_type;
static QueueHandle_t s_txq, s_rxq;

static volatile uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static volatile bool s_encrypted;
static volatile bool s_ready;        /* BLEとして送受信可（暗号化＋購読済み） */
static volatile bool s_up;           /* yd_sec の相互認証まで完了 */
static volatile bool s_subscribed;   /* peripheral: 相手が通知を購読中 */

/* peripheral */
static uint16_t s_to_desk_val, s_to_recv_val;      /* GATTサーバーのハンドル */
/* central */
static uint16_t s_svc_start, s_svc_end;
static uint16_t s_peer_to_desk, s_peer_to_recv;
static bool s_have_target;

/* いま接続している相手（暗号化できた時点で記録）。PSK の相互認証に失敗した相手と、
 * この接続で新しくペアリングしたのに認証まで進まなかった相手は、切断時に BLE のボンド情報も消す
 * （yd_bond_policy.h）。鍵を持たない偽の機器に「登録済みの相手」や新規ペアリングの枠を
 * 取られたままにしないため */
static ble_addr_t s_peer_addr;
static volatile bool s_peer_addr_valid;
static volatile bool s_forget_on_disconnect;   /* 認証前の失敗で、こちらから切った */
static volatile bool s_conn_was_bonded;        /* 接続した時点で、この相手のボンドがあった */
static volatile bool s_conn_authed;            /* この接続で相互認証が通った */
static volatile uint32_t s_ev_drops;           /* 受信キューに状態の通知を入れられなかった回数 */
static ble_addr_t s_target;

/* ======================================================================
 *  接続パラメータの省電力切り替え（Peripheral = 机上ユニットのみ）
 *
 *  通信中 : 間隔15〜30ms / レイテンシ0         → 低遅延
 *  無通信 : 間隔30ms / レイテンシ12            → 実質 (1+12)*30 = 390ms ごとにだけ起きる
 *           スーパービジョンタイムアウト 6s（≥ (1+12)*30ms*2 = 780ms かつ ≥ 4s）
 *  送受信があれば即座に「通信中」を要求し、CP_IDLE_AFTER_MS 無通信で「無通信」へ戻す。
 *  Central（レシーバー）は NimBLE 既定どおり相手の要求をそのまま受け入れる。
 * ====================================================================== */
#define CP_ACTIVE_ITVL_MIN_MS   15
#define CP_ACTIVE_ITVL_MAX_MS   30
#define CP_ACTIVE_LATENCY       0
#define CP_ACTIVE_TIMEOUT_MS    4000
#define CP_IDLE_ITVL_MS         30
#define CP_IDLE_LATENCY         12
#define CP_IDLE_TIMEOUT_MS      6000
#define CP_IDLE_AFTER_MS        3000     /* この時間無通信なら省電力パラメータへ */
#define CP_RETRY_MS             10000    /* 更新が失敗/拒否されたら次に試すまでの間隔 */
#define CP_TICK_MS              1000     /* 判定周期（tx_task のキュー待ちタイムアウト） */

static portMUX_TYPE s_cp_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_cp_enabled;                /* ble_conn_power_save && Peripheral */
static volatile bool s_cp_idle;          /* いま適用されているのが省電力パラメータか */
static volatile bool s_cp_want_idle;     /* 目標 */
static volatile bool s_cp_pending;       /* 更新手続き中 */
static int64_t s_cp_last_traffic_us;
static int64_t s_cp_retry_at_us;

static void cp_request(bool idle)
{
    struct ble_gap_upd_params p = { 0 };
    if (idle) {
        p.itvl_min = BLE_GAP_CONN_ITVL_MS(CP_IDLE_ITVL_MS);
        p.itvl_max = BLE_GAP_CONN_ITVL_MS(CP_IDLE_ITVL_MS);
        p.latency = CP_IDLE_LATENCY;
        p.supervision_timeout = BLE_GAP_SUPERVISION_TIMEOUT_MS(CP_IDLE_TIMEOUT_MS);
    } else {
        p.itvl_min = BLE_GAP_CONN_ITVL_MS(CP_ACTIVE_ITVL_MIN_MS);
        p.itvl_max = BLE_GAP_CONN_ITVL_MS(CP_ACTIVE_ITVL_MAX_MS);
        p.latency = CP_ACTIVE_LATENCY;
        p.supervision_timeout = BLE_GAP_SUPERVISION_TIMEOUT_MS(CP_ACTIVE_TIMEOUT_MS);
    }
    uint16_t conn = s_conn;
    int rc = (conn == BLE_HS_CONN_HANDLE_NONE) ? BLE_HS_ENOTCONN : ble_gap_update_params(conn, &p);
    if (rc == 0) {
        ESP_LOGI(TAG, "接続パラメータ変更を要求: %s", idle ? "省電力（30ms / latency 12 / 6s）" : "低遅延（15-30ms / latency 0 / 4s）");
        return;
    }
    portENTER_CRITICAL(&s_cp_mux);
    s_cp_pending = false;
    /* 低遅延への切り替えはすぐ再試行、省電力側は少し待つ */
    s_cp_retry_at_us = esp_timer_get_time() + (idle ? (int64_t)CP_RETRY_MS * 1000 : 200 * 1000);
    portEXIT_CRITICAL(&s_cp_mux);
    ESP_LOGW(TAG, "接続パラメータ変更の要求に失敗 rc=%d（後で再試行）", rc);
}

/* 目標と現状が違えば更新を要求する（どのタスクから呼んでもよい） */
static void cp_eval(void)
{
    if (!s_cp_enabled || !s_up) {
        return;
    }
    bool req = false, want = false;
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_cp_mux);
    if (!s_cp_want_idle && (now - s_cp_last_traffic_us) >= (int64_t)CP_IDLE_AFTER_MS * 1000) {
        s_cp_want_idle = true;
    }
    want = s_cp_want_idle;
    if (want != s_cp_idle && !s_cp_pending && (!want || now >= s_cp_retry_at_us)) {
        s_cp_pending = true;
        req = true;
    }
    portEXIT_CRITICAL(&s_cp_mux);
    if (req) {
        cp_request(want);
    }
}

/* 送受信のたびに呼ぶ。省電力側なら低遅延側へ戻す */
static void cp_on_traffic(void)
{
    if (!s_cp_enabled) {
        return;
    }
    portENTER_CRITICAL(&s_cp_mux);
    s_cp_last_traffic_us = esp_timer_get_time();
    s_cp_want_idle = false;
    bool need = s_cp_idle && !s_cp_pending;
    portEXIT_CRITICAL(&s_cp_mux);
    if (need) {
        cp_eval();
    }
}

static void cp_reset(bool idle_now)
{
    portENTER_CRITICAL(&s_cp_mux);
    s_cp_idle = idle_now;
    s_cp_want_idle = false;
    s_cp_pending = false;
    s_cp_last_traffic_us = esp_timer_get_time();
    s_cp_retry_at_us = 0;
    portEXIT_CRITICAL(&s_cp_mux);
}

static void log_conn_params(const char *label, uint16_t conn)
{
    struct ble_gap_conn_desc d;
    if (ble_gap_conn_find(conn, &d) != 0) {
        return;
    }
    unsigned itvl_x100 = (unsigned)d.conn_itvl * 125;   /* 1.25ms単位 → 0.01ms単位 */
    ESP_LOGI(TAG, "%s interval=%u.%02ums latency=%u timeout=%ums（実質の起床間隔 約%ums）",
             label, itvl_x100 / 100, itvl_x100 % 100, d.conn_latency, (unsigned)d.supervision_timeout * 10,
             (unsigned)(itvl_x100 * (1u + d.conn_latency)) / 100);
}

void ydl_ble_get_conn_info(yd_link_conn_info_t *out)
{
    struct ble_gap_conn_desc d;
    uint16_t conn = s_conn;
    out->connected = s_up;
    out->power_save = s_cp_idle;
    out->update_pending = s_cp_pending;
    if (conn != BLE_HS_CONN_HANDLE_NONE && ble_gap_conn_find(conn, &d) == 0) {
        out->itvl = d.conn_itvl;
        out->latency = d.conn_latency;
        out->timeout = d.supervision_timeout;
    }
}

static void set_up(bool up)
{
    if (s_up == up) {
        return;
    }
    s_up = up;
    ESP_LOGI(TAG, "%s", up ? "▶ BLEリンク確立（暗号化＋鍵による相互認証済み）" : "■ BLEリンク切断");
    if (up && s_cp_enabled) {
        /* 確立直後はレシーバーが指定した低遅延パラメータ。CP_IDLE_AFTER_MS 後に省電力へ */
        struct ble_gap_conn_desc d;
        cp_reset(s_conn != BLE_HS_CONN_HANDLE_NONE && ble_gap_conn_find(s_conn, &d) == 0 && d.conn_latency > 0);
    }
    if (s_cfg.state_cb) {
        s_cfg.state_cb(up, s_cfg.state_cb_arg);
    }
}

static void print_addr(const char *label, const ble_addr_t *a)
{
    const uint8_t *v = a->val;
    ESP_LOGI(TAG, "%s %02x:%02x:%02x:%02x:%02x:%02x", label, v[5], v[4], v[3], v[2], v[1], v[0]);
}

static int bond_count(void)
{
    int n = 0;
    ble_store_util_count(BLE_STORE_OBJ_TYPE_OUR_SEC, &n);
    return n;
}

static bool peer_is_bonded(const ble_addr_t *addr)
{
    struct ble_store_key_sec key = { .peer_addr = *addr };
    struct ble_store_value_sec val;
    return ble_store_read_peer_sec(&key, &val) == 0;
}

static bool pairing_window_open(void)
{
    return bond_count() == 0 || esp_timer_get_time() < (int64_t)PAIRING_WINDOW_S * 1000000;
}

/* ---- NVS: レシーバーが覚える相手アドレス ---- */
static void target_load(void)
{
    nvs_handle_t h;
    if (nvs_open("yd_link", NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t sz = sizeof(s_target);
    s_have_target = (nvs_get_blob(h, "target", &s_target, &sz) == ESP_OK && sz == sizeof(s_target));
    nvs_close(h);
}

static void target_save(const ble_addr_t *a)
{
    nvs_handle_t h;
    if (nvs_open("yd_link", NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_blob(h, "target", a, sizeof(*a));
    nvs_commit(h);
    nvs_close(h);
    s_target = *a;
    s_have_target = true;
}

/* ======================================================================
 *  受信・送信タスク（NimBLEホストタスクを塞がないため）
 * ====================================================================== */
/* ======================================================================
 *  yd_sec: 事前共有鍵による相互認証と暗号化（ハンドシェイクは rx_task だけが触る）
 * ====================================================================== */
static uint8_t s_psk[YD_SEC_KEY_LEN];
static bool s_have_psk;
static SemaphoreHandle_t s_sec_mutex;      /* tx_task（暗号化）と rx_task（鍵の更新）の排他 */
static yd_sec_keys_t s_keys;
static uint8_t s_my_nonce[YD_SEC_NONCE_LEN], s_peer_nonce[YD_SEC_NONCE_LEN];
static uint8_t s_peer_mac[YD_SEC_MAC_LEN];
static bool s_hs_hello_sent, s_hs_peer_hello, s_hs_keys, s_hs_auth_sent, s_hs_peer_auth;
static uint32_t s_tx_ctr, s_rx_ctr;
static esp_timer_handle_t s_hs_timer;
static uint32_t s_sec_fail;

/* 接続・切断の通知。受信データと同じキューに入れて順番を保つ（切断より前のデータは、
 * 切断の処理より先に読まれる）。push_rx は最後の RXQ_EV_RESERVE 枠を使わないので、相手が
 * データを送りつけてキューを埋めても、通知は入る。どちらも NimBLE のホストタスクからしか
 * 呼ばれないので、空きの確認と投入の間に割り込まれることはない */
static void post_event(pk_kind_t kind)
{
    pkt_t p = { .kind = kind, .len = 0 };
    if (xQueueSend(s_rxq, &p, pdMS_TO_TICKS(100)) != pdTRUE) {
        s_ev_drops++;
        ESP_LOGE(TAG, "接続状態の通知をキューに入れられませんでした（%" PRIu32 "回目）→ 切断します", s_ev_drops);
        uint16_t c = s_conn;
        if (c != BLE_HS_CONN_HANDLE_NONE) {
            ble_gap_terminate(c, BLE_ERR_REM_USER_CONN_TERM);
        }
    }
}

/* BLE層の「送受信できる / できない」。アプリへの通知はハンドシェイク完了後 */
static void transport_ready(bool ready)
{
    if (ready && s_ready) {
        return;
    }
    /* 切断は毎回通知する（準備完了前に届いたHELLOなどの状態も確実に消すため） */
    s_ready = ready;
    if (!ready) {
        set_up(false);              /* 先に送信を止める */
        if (s_hs_timer) {
            esp_timer_stop(s_hs_timer);
        }
    }
    post_event(ready ? PK_EV_READY : PK_EV_DOWN);
}

static void hs_disconnect(const char *why)
{
    ESP_LOGE(TAG, "%s → 切断します", why);
    if (!s_up) {
        /* 鍵の相互認証が済む前の切断（不一致・時間切れ・形のおかしい HELLO/AUTH など）は理由を問わず、
         * 相手の BLE ボンドも消す。正しい相手ならつなぎ直したときに自動でペアリングし直す */
        s_forget_on_disconnect = true;
    }
    uint16_t c = s_conn;
    if (c != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(c, BLE_ERR_AUTH_FAIL);
    }
}

static void hs_timeout_cb(void *arg)
{
    if (s_ready && !s_up) {
        hs_disconnect("鍵による相互認証が時間内に終わりませんでした（相手のファームが古いか、鍵がない可能性）");
    }
}

static void hs_reset(void)
{
    xSemaphoreTake(s_sec_mutex, portMAX_DELAY);
    yd_sec_wipe(&s_keys, sizeof(s_keys));
    s_tx_ctr = s_rx_ctr = 0;
    xSemaphoreGive(s_sec_mutex);
    yd_sec_wipe(s_my_nonce, sizeof(s_my_nonce));
    yd_sec_wipe(s_peer_nonce, sizeof(s_peer_nonce));
    s_hs_hello_sent = s_hs_peer_hello = s_hs_keys = s_hs_auth_sent = s_hs_peer_auth = false;
}

static void send_raw(const uint8_t *buf, uint16_t len)
{
    pkt_t p = { .kind = PK_RAW, .len = len };
    memcpy(p.buf, buf, len);
    xQueueSend(s_txq, &p, pdMS_TO_TICKS(100));
}

static uint8_t my_role(void) { return s_is_central ? YD_SEC_ROLE_CENTRAL : YD_SEC_ROLE_PERIPHERAL; }
static uint8_t peer_role(void) { return s_is_central ? YD_SEC_ROLE_PERIPHERAL : YD_SEC_ROLE_CENTRAL; }
static const uint8_t *nonce_c(void) { return s_is_central ? s_my_nonce : s_peer_nonce; }
static const uint8_t *nonce_p(void) { return s_is_central ? s_peer_nonce : s_my_nonce; }

/* 揃った材料に応じて先へ進める */
static void hs_progress(void)
{
    if (s_up || !s_ready) {
        return;
    }
    if (s_hs_hello_sent && s_hs_peer_hello && !s_hs_keys) {
        xSemaphoreTake(s_sec_mutex, portMAX_DELAY);
        int rc = yd_sec_derive(s_psk, nonce_c(), nonce_p(), &s_keys);
        s_tx_ctr = s_rx_ctr = 0;
        xSemaphoreGive(s_sec_mutex);
        if (rc != 0) {
            hs_disconnect("鍵の導出に失敗");
            return;
        }
        s_hs_keys = true;
    }
    if (s_hs_keys && !s_hs_auth_sent) {
        uint8_t m[1 + YD_SEC_MAC_LEN] = { YD_SEC_T_AUTH };
        yd_sec_auth_mac(&s_keys, my_role(), nonce_c(), nonce_p(), &m[1]);
        send_raw(m, sizeof(m));
        s_hs_auth_sent = true;
    }
    if (s_hs_keys && s_hs_auth_sent && s_hs_peer_auth) {
        uint8_t expect[YD_SEC_MAC_LEN];
        yd_sec_auth_mac(&s_keys, peer_role(), nonce_c(), nonce_p(), expect);
        bool ok = yd_sec_equal(expect, s_peer_mac, YD_SEC_MAC_LEN);
        yd_sec_wipe(expect, sizeof(expect));
        if (!ok) {
            s_sec_fail++;
            hs_disconnect("相手の鍵が一致しません。2台に同じ鍵ファイルを書き込んでください");
            return;
        }
        esp_timer_stop(s_hs_timer);
        s_conn_authed = true;
        if (s_is_central && !s_have_target && s_peer_addr_valid) {
            target_save(&s_peer_addr);   /* 鍵の相互認証が通った相手だけを登録する */
            print_addr("この机上ユニットを登録しました:", &s_peer_addr);
        }
        set_up(true);
    }
}

static void send_hello(void)
{
    esp_fill_random(s_my_nonce, sizeof(s_my_nonce));
    uint8_t m[3 + YD_SEC_NONCE_LEN] = { YD_SEC_T_HELLO, YD_SEC_VERSION, my_role() };
    memcpy(&m[3], s_my_nonce, YD_SEC_NONCE_LEN);
    send_raw(m, sizeof(m));
    s_hs_hello_sent = true;
}

/* 状態は EV_DOWN（切断）でだけ消す。相手のHELLOが先に届いていることがあるため。
 * HELLO はレシーバー（Central）から始める。Central はサービス探索と購読を終えてから送るので、
 * 机上ユニットの返事を取りこぼさない。机上ユニットは HELLO を受け取ってから返す（on_packet） */
static void hs_begin(void)
{
    if (!s_have_psk) {
        hs_disconnect("鍵（keyrelay-key.bin）が書き込まれていません");
        return;
    }
    if (s_is_central && !s_hs_hello_sent) {
        send_hello();
    }
    esp_timer_stop(s_hs_timer);
    esp_timer_start_once(s_hs_timer, (uint64_t)HS_TIMEOUT_MS * 1000);
    hs_progress();
}

/* 相手から届いた1パケット */
static void on_packet(const pkt_t *p)
{
    static uint8_t pt[BLE_PKT_MAX];
    if (s_conn == BLE_HS_CONN_HANDLE_NONE || p->len < 1) {
        return;
    }
    switch (p->buf[0]) {
    case YD_SEC_T_HELLO:
        if (p->len != 3 + YD_SEC_NONCE_LEN || p->buf[1] != YD_SEC_VERSION || p->buf[2] != peer_role() || s_hs_peer_hello) {
            hs_disconnect("不正なHELLO");
            return;
        }
        memcpy(s_peer_nonce, &p->buf[3], YD_SEC_NONCE_LEN);
        s_hs_peer_hello = true;
        if (!s_hs_hello_sent && s_ready && s_have_psk) {
            send_hello();       /* 机上ユニット: 受け取ってから返す */
        }
        hs_progress();
        return;
    case YD_SEC_T_AUTH:
        if (p->len != 1 + YD_SEC_MAC_LEN || s_hs_peer_auth) {
            hs_disconnect("不正なAUTH");
            return;
        }
        memcpy(s_peer_mac, &p->buf[1], YD_SEC_MAC_LEN);
        s_hs_peer_auth = true;
        hs_progress();
        return;
    case YD_SEC_T_DATA: {
        if (!s_up) {
            return;
        }
        uint32_t ctr = 0;
        xSemaphoreTake(s_sec_mutex, portMAX_DELAY);
        int n = yd_sec_open(s_is_central ? s_keys.k_p2c : s_keys.k_c2p, p->buf, p->len, &ctr, pt, sizeof(pt));
        bool fresh = (n >= 1) && ctr > s_rx_ctr;
        if (fresh) {
            s_rx_ctr = ctr;
        }
        xSemaphoreGive(s_sec_mutex);
        if (n < 1) {
            s_sec_fail++;
            hs_disconnect("改ざんされたか壊れたデータを受信");
            return;
        }
        if (!fresh) {
            s_sec_fail++;
            hs_disconnect("古い通し番号のデータを受信（リプレイの疑い）");
            return;
        }
        g_ydl_stats.rx_frames++;
        if (s_cfg.rx_cb) {
            s_cfg.rx_cb(pt[0], &pt[1], (uint16_t)(n - 1), s_cfg.rx_cb_arg);
        }
        return;
    }
    default:
        hs_disconnect("暗号化されていないデータを受信（相手のファームが古い可能性）");
        return;
    }
}

static bool load_psk(void)
{
    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, KEY_PARTITION);
    if (!part) {
        ESP_LOGE(TAG, "パーティション \"%s\" がありません（パーティションテーブルが古い）", KEY_PARTITION);
        return false;
    }
    uint8_t buf[YD_SEC_KEYFILE_LEN];
    if (esp_partition_read(part, 0, buf, sizeof(buf)) != ESP_OK) {
        return false;
    }
    bool ok = yd_sec_parse_keyfile(buf, sizeof(buf), s_psk);
    yd_sec_wipe(buf, sizeof(buf));
    if (ok) {
        ESP_LOGI(TAG, "鍵を読み込みました（0x%06" PRIx32 "）", part->address);
    } else {
        ESP_LOGE(TAG, "鍵がありません。鍵ファイル（keyrelay-key.bin）を 0x%06" PRIx32 " に書き込んでください（docs/BUILD.md 参照）", part->address);
    }
    return ok;
}

/* ======================================================================
 *  受信・送信タスク（NimBLEホストタスクを塞がないため）
 * ====================================================================== */
static void push_rx(struct os_mbuf *om)
{
    pkt_t p = { .kind = PK_DATA };
    uint16_t n = 0;
    if (ble_hs_mbuf_to_flat(om, p.buf, sizeof(p.buf), &n) != 0 || n < 1) {
        g_ydl_stats.rx_overflows++;
        return;
    }
    p.len = n;
    if (uxQueueSpacesAvailable(s_rxq) <= RXQ_EV_RESERVE || xQueueSend(s_rxq, &p, 0) != pdTRUE) {
        g_ydl_stats.rx_overflows++;
    }
}

static void rx_task(void *arg)
{
    static pkt_t p;
    for (;;) {
        xQueueReceive(s_rxq, &p, portMAX_DELAY);
        switch (p.kind) {
        case PK_EV_READY: hs_begin(); break;
        case PK_EV_DOWN:  hs_reset(); break;
        default:          on_packet(&p); break;
        }
    }
}

static int send_one(const uint8_t *buf, uint16_t len)
{
    if (s_is_central) {
        return ble_gattc_write_no_rsp_flat(s_conn, s_peer_to_desk, buf, len);
    }
    struct os_mbuf *om = ble_hs_mbuf_from_flat(buf, len);
    if (!om) {
        return BLE_HS_ENOMEM;
    }
    return ble_gatts_notify_custom(s_conn, s_to_recv_val, om);   /* omは成否に関わらず消費される */
}

static void tx_task(void *arg)
{
    static pkt_t p;
    static uint8_t out[BLE_PKT_MAX];
    for (;;) {
        if (xQueueReceive(s_txq, &p, s_cp_enabled ? pdMS_TO_TICKS(CP_TICK_MS) : portMAX_DELAY) != pdTRUE) {
            cp_eval();   /* 無通信が続いていれば省電力パラメータへ */
            continue;
        }
        const uint8_t *buf = p.buf;
        uint16_t len = p.len;
        if (p.kind == PK_RAW) {
            if (!s_ready) {
                continue;
            }
        } else {
            if (!s_up) {
                g_ydl_stats.tx_dropped++;
                continue;
            }
            xSemaphoreTake(s_sec_mutex, portMAX_DELAY);
            int n = -1;
            if (s_tx_ctr < UINT32_MAX) {
                n = yd_sec_seal(s_is_central ? s_keys.k_c2p : s_keys.k_p2c, ++s_tx_ctr, p.buf, p.len, out, sizeof(out));
            }
            xSemaphoreGive(s_sec_mutex);
            if (n < 0) {
                g_ydl_stats.tx_dropped++;
                hs_disconnect("暗号化に失敗");
                continue;
            }
            buf = out;
            len = (uint16_t)n;
        }
        int rc = 0;
        for (int retry = 0; retry < 200; retry++) {      /* 送信バッファが空くまで最大約400ms */
            rc = send_one(buf, len);
            if (rc != BLE_HS_ENOMEM && rc != BLE_HS_EBUSY) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(2) ? pdMS_TO_TICKS(2) : 1);   /* 100Hz tickでも必ず1tick待つ */
        }
        if (rc == 0) {
            if (p.kind != PK_RAW) {
                g_ydl_stats.tx_frames++;
            }
        } else {
            g_ydl_stats.tx_dropped++;
            ESP_LOGW(TAG, "送信失敗 rc=%d", rc);
        }
    }
}

esp_err_t ydl_ble_send(uint8_t ch, const uint8_t *data, uint16_t len)
{
    if (!s_txq) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_up) {
        return ESP_ERR_INVALID_STATE;
    }
    if (len > BLE_PKT_MAX - 1 - YD_SEC_OVERHEAD) {   /* 16bit の足し算があふれる前に弾く */
        ESP_LOGE(TAG, "ペイロードが大きすぎます len=%u", len);
        return ESP_ERR_INVALID_SIZE;
    }
    uint16_t mtu = ble_att_mtu(s_conn);
    size_t wire = (size_t)len + 1 + YD_SEC_OVERHEAD;
    if (mtu && wire > (size_t)mtu - 3) {
        ESP_LOGE(TAG, "ペイロードが大きすぎます len=%u mtu=%u", len, mtu);
        return ESP_ERR_INVALID_SIZE;
    }
    pkt_t p = { .kind = PK_DATA, .len = len + 1 };
    p.buf[0] = ch;
    memcpy(&p.buf[1], data, len);
    cp_on_traffic();
    return xQueueSend(s_txq, &p, pdMS_TO_TICKS(50)) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

uint16_t ydl_ble_max_payload(void)
{
    if (!s_up) {
        return 0;
    }
    uint16_t mtu = ble_att_mtu(s_conn);
    uint16_t wire = BLE_PKT_MAX;
    if (mtu && mtu - 3 < wire) {
        wire = mtu - 3;
    }
    return wire > 1 + YD_SEC_OVERHEAD ? wire - 1 - YD_SEC_OVERHEAD : 0;
}

bool ydl_ble_is_up(void)
{
    return s_up;
}

esp_err_t ydl_ble_forget_peers(void)
{
    ble_store_clear();
    nvs_handle_t h;
    if (nvs_open("yd_link", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    s_have_target = false;
    ESP_LOGW(TAG, "ペアリング情報を消去しました");
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
    }
    return ESP_OK;
}

/* ======================================================================
 *  Peripheral（机上ユニット）
 * ====================================================================== */
static int gap_event_periph(struct ble_gap_event *event, void *arg);

static int chr_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR && attr_handle == s_to_desk_val) {
        if (!s_ready) {
            return BLE_ATT_ERR_INSUFFICIENT_ENC;
        }
        push_rx(ctxt->om);
        cp_on_traffic();   /* レシーバーから要求が来た → 低遅延パラメータへ */
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def s_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &UUID_SVC.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &UUID_TO_DESK.u,
                .access_cb = chr_access,
                .flags = BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE_ENC,
                .val_handle = &s_to_desk_val,
            },
            {
                .uuid = &UUID_TO_RECV.u,
                .access_cb = chr_access,
                .flags = BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_to_recv_val,
            },
            { 0 },
        },
    },
    { 0 },
};

static void periph_advertise(void)
{
    struct ble_hs_adv_fields f = { 0 };
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.uuids128 = (ble_uuid128_t *)&UUID_SVC;
    f.num_uuids128 = 1;
    f.uuids128_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&f);
    if (rc) {
        ESP_LOGE(TAG, "adv fields rc=%d", rc);
        return;
    }
    struct ble_hs_adv_fields rsp = { 0 };
    rsp.name = (uint8_t *)DEVICE_NAME;
    rsp.name_len = strlen(DEVICE_NAME);
    rsp.name_is_complete = 1;
    ble_gap_adv_rsp_set_fields(&rsp);

    struct ble_gap_adv_params p = { 0 };
    p.conn_mode = BLE_GAP_CONN_MODE_UND;
    p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    p.itvl_min = BLE_GAP_ADV_ITVL_MS(100);
    p.itvl_max = BLE_GAP_ADV_ITVL_MS(150);
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &p, gap_event_periph, NULL);
    if (rc && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "adv start rc=%d", rc);
    } else {
        ESP_LOGI(TAG, "アドバタイズ中…（新規ペアリング受付: %s）", pairing_window_open() ? "可" : "不可");
    }
}

/* 接続した直後に呼ぶ。この接続の前からボンドがあった相手かを覚えておく */
static void conn_begin(uint16_t conn)
{
    struct ble_gap_conn_desc desc;
    s_forget_on_disconnect = false;
    s_conn_authed = false;
    s_peer_addr_valid = false;
    s_conn_was_bonded = ble_gap_conn_find(conn, &desc) == 0 && peer_is_bonded(&desc.peer_id_addr);
}

static void forget_failed_peer(void)
{
    yd_bond_conn_t c = {
        .peer_known = s_peer_addr_valid,
        .bonded_before_conn = s_conn_was_bonded,
        .authed = s_conn_authed,
        .hs_failed = s_forget_on_disconnect,
    };
    if (yd_bond_forget_on_disconnect(&c)) {
        ble_store_util_delete_peer(&s_peer_addr);
        print_addr(c.hs_failed ? "鍵の相互認証に失敗した相手のペアリング情報を消しました:"
                               : "ペアリングだけして認証前に切れた相手のペアリング情報を消しました:",
                   &s_peer_addr);
    }
    s_forget_on_disconnect = false;
    s_conn_authed = false;
    s_conn_was_bonded = false;
    s_peer_addr_valid = false;
}

/* ボンドの保存先がいっぱいのとき。NimBLE 標準の ble_store_util_status_rr は一番古い相手を
 * 消して場所を空けるが、それだと受付時間中にペアリングを繰り返すだけで、正規の相手を
 * 追い出せてしまう。ここでは古い相手を消さず、新しい保存のほうを失敗させる */
static int store_status_cb(struct ble_store_status_event *event, void *arg)
{
    switch (event->event_code) {
    case BLE_STORE_EVENT_OVERFLOW:
        ESP_LOGW(TAG, "ペアリング情報の保存先がいっぱいです（obj_type=%d）。古い相手は消さず、今回の保存をあきらめます。"
                      "相手を入れ替えるときは BOOT を3秒押してペアリング情報を消してください",
                 event->overflow.obj_type);
        return BLE_HS_ESTORE_CAP;
    case BLE_STORE_EVENT_FULL:
        return 0;
    default:
        return BLE_HS_EUNKNOWN;
    }
}

static int gap_event_periph(struct ble_gap_event *event, void *arg)
{
    struct ble_gap_conn_desc desc;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            periph_advertise();
            return 0;
        }
        s_conn = event->connect.conn_handle;
        conn_begin(s_conn);
        s_encrypted = false;
        s_subscribed = false;
        cp_reset(false);
        log_conn_params("接続パラメータ（初期）:", s_conn);
        if (ble_gap_conn_find(s_conn, &desc) == 0) {
            print_addr("接続:", &desc.peer_id_addr);
            if (!pairing_window_open() && !peer_is_bonded(&desc.peer_id_addr)) {
                ESP_LOGW(TAG, "未登録の相手です。ペアリング受付時間外のため切断します");
                ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
            }
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "切断 reason=0x%x", event->disconnect.reason);
        forget_failed_peer();
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_encrypted = false;
        s_subscribed = false;
        transport_ready(false);
        cp_reset(false);
        periph_advertise();
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        if (event->enc_change.status == 0 && ble_gap_conn_find(event->enc_change.conn_handle, &desc) == 0) {
            s_encrypted = desc.sec_state.encrypted;
            ESP_LOGI(TAG, "暗号化 %s（bonded=%d）", s_encrypted ? "OK" : "NG", desc.sec_state.bonded);
            s_peer_addr = desc.peer_id_addr;
            s_peer_addr_valid = true;
            /* ボンディング済みの相手は購読状態が復元済みのことがある */
            if (s_encrypted && s_subscribed) {
                transport_ready(true);
            }
        } else {
            ESP_LOGW(TAG, "暗号化失敗 status=%d", event->enc_change.status);
        }
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_to_recv_val) {
            s_subscribed = event->subscribe.cur_notify;
            if (s_subscribed && s_encrypted) {
                transport_ready(true);
            } else if (!s_subscribed) {
                transport_ready(false);
            }
        }
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU=%d", event->mtu.value);
        return 0;

    case BLE_GAP_EVENT_CONN_UPDATE: {
        bool ok = (event->conn_update.status == 0);
        bool idle_now = s_cp_idle;
        if (ok && ble_gap_conn_find(event->conn_update.conn_handle, &desc) == 0) {
            idle_now = desc.conn_latency > 0;
        }
        portENTER_CRITICAL(&s_cp_mux);
        s_cp_pending = false;
        s_cp_idle = idle_now;
        if (!ok) {
            s_cp_retry_at_us = esp_timer_get_time() + (int64_t)CP_RETRY_MS * 1000;
        }
        portEXIT_CRITICAL(&s_cp_mux);
        if (ok) {
            log_conn_params(idle_now ? "接続パラメータ更新完了（省電力）:" : "接続パラメータ更新完了（低遅延）:",
                            event->conn_update.conn_handle);
        } else {
            ESP_LOGW(TAG, "接続パラメータ更新に失敗 status=%d（%d秒後に再試行）", event->conn_update.status, CP_RETRY_MS / 1000);
        }
        /* 手続き中に通信が始まっていたら、すぐ低遅延へ戻す */
        cp_eval();
        return 0;
    }

    case BLE_GAP_EVENT_CONN_UPDATE_REQ:
        /* レシーバー側から変更要求が来た場合はそのまま受け入れる */
        ESP_LOGI(TAG, "相手から接続パラメータ変更要求: interval %u-%u latency %u timeout %u",
                 event->conn_update_req.peer_params->itvl_min, event->conn_update_req.peer_params->itvl_max,
                 event->conn_update_req.peer_params->latency, event->conn_update_req.peer_params->supervision_timeout);
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        if (!pairing_window_open()) {
            ESP_LOGW(TAG, "再ペアリング要求を拒否（受付時間外）");
            return BLE_GAP_REPEAT_PAIRING_IGNORE;
        }
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        s_conn_was_bonded = false;   /* 古いボンドを消して、この接続で作り直す */
        return BLE_GAP_REPEAT_PAIRING_RETRY;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        periph_advertise();
        return 0;
    }
    return 0;
}

/* ======================================================================
 *  Central（レシーバー）
 * ====================================================================== */
static int gap_event_central(struct ble_gap_event *event, void *arg);

static void central_scan(void)
{
    struct ble_gap_disc_params dp = { 0 };
    dp.filter_duplicates = 1;
    dp.passive = 1;
    int rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &dp, gap_event_central, NULL);
    if (rc && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "scan start rc=%d", rc);
    } else {
        ESP_LOGI(TAG, "机上ユニットを探しています…%s", s_have_target ? "（登録済みの相手のみ）" : "（初回ペアリング）");
    }
}

static bool adv_has_our_svc(const struct ble_gap_disc_desc *d)
{
    struct ble_hs_adv_fields f;
    if (ble_hs_adv_parse_fields(&f, d->data, d->length_data) != 0) {
        return false;
    }
    for (int i = 0; i < f.num_uuids128; i++) {
        if (ble_uuid_cmp(&f.uuids128[i].u, &UUID_SVC.u) == 0) {
            return true;
        }
    }
    return false;
}

static void central_connect(const ble_addr_t *addr)
{
    if (ble_gap_disc_cancel() != 0) {
        /* すでに止まっていてもOK */
    }
    struct ble_gap_conn_params cp = {
        .scan_itvl = 0x10, .scan_window = 0x10,
        .itvl_min = BLE_GAP_CONN_ITVL_MS(15), .itvl_max = BLE_GAP_CONN_ITVL_MS(30),
        .latency = 0,
        .supervision_timeout = BLE_GAP_SUPERVISION_TIMEOUT_MS(4000),
        .min_ce_len = 0, .max_ce_len = 0,
    };
    print_addr("接続を試みます:", addr);
    int rc = ble_gap_connect(s_own_addr_type, addr, 10000, &cp, gap_event_central, NULL);
    if (rc) {
        ESP_LOGW(TAG, "connect rc=%d", rc);
        central_scan();
    }
}

static int on_cccd_written(uint16_t conn, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg)
{
    if (err->status == 0) {
        transport_ready(true);
    } else {
        ESP_LOGE(TAG, "通知の購読に失敗 status=%d", err->status);
        ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
    }
    return 0;
}

static int on_chr(uint16_t conn, const struct ble_gatt_error *err, const struct ble_gatt_chr *chr, void *arg)
{
    if (err->status == 0) {
        if (ble_uuid_cmp(&chr->uuid.u, &UUID_TO_DESK.u) == 0) {
            s_peer_to_desk = chr->val_handle;
        } else if (ble_uuid_cmp(&chr->uuid.u, &UUID_TO_RECV.u) == 0) {
            s_peer_to_recv = chr->val_handle;
        }
        return 0;
    }
    if (err->status == BLE_HS_EDONE) {
        if (!s_peer_to_desk || !s_peer_to_recv) {
            ESP_LOGE(TAG, "キャラクタリスティックが見つかりません");
            ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
            return 0;
        }
        /* NimBLEのGATTサーバーでは CCCD は Notify値ハンドルの直後に置かれる */
        static const uint8_t on[2] = { 0x01, 0x00 };
        ble_gattc_write_flat(conn, s_peer_to_recv + 1, on, sizeof(on), on_cccd_written, NULL);
        return 0;
    }
    ESP_LOGE(TAG, "キャラクタリスティック探索エラー %d", err->status);
    ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
    return 0;
}

static int on_svc(uint16_t conn, const struct ble_gatt_error *err, const struct ble_gatt_svc *svc, void *arg)
{
    if (err->status == 0) {
        s_svc_start = svc->start_handle;
        s_svc_end = svc->end_handle;
        return 0;
    }
    if (err->status == BLE_HS_EDONE && s_svc_start) {
        ble_gattc_disc_all_chrs(conn, s_svc_start, s_svc_end, on_chr, NULL);
        return 0;
    }
    ESP_LOGE(TAG, "サービスが見つかりません %d", err->status);
    ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
    return 0;
}

static int on_mtu(uint16_t conn, const struct ble_gatt_error *err, uint16_t mtu, void *arg)
{
    ESP_LOGI(TAG, "MTU=%u", mtu);
    return 0;
}

static int gap_event_central(struct ble_gap_event *event, void *arg)
{
    struct ble_gap_conn_desc desc;
    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        if (!adv_has_our_svc(&event->disc)) {
            return 0;
        }
        if (s_have_target && ble_addr_cmp(&event->disc.addr, &s_target) != 0) {
            return 0;   /* 登録済みの机上ユニット以外は無視 */
        }
        central_connect(&event->disc.addr);
        return 0;

    case BLE_GAP_EVENT_DISC_COMPLETE:
        central_scan();
        return 0;

    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            ESP_LOGW(TAG, "接続失敗 status=%d", event->connect.status);
            central_scan();
            return 0;
        }
        s_conn = event->connect.conn_handle;
        conn_begin(s_conn);
        s_encrypted = false;
        s_svc_start = s_svc_end = s_peer_to_desk = s_peer_to_recv = 0;
        ble_gattc_exchange_mtu(s_conn, on_mtu, NULL);
        /* 既存の鍵があれば暗号化、なければペアリング */
        ble_gap_security_initiate(s_conn);
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "切断 reason=0x%x", event->disconnect.reason);
        forget_failed_peer();
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_encrypted = false;
        transport_ready(false);
        central_scan();
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        if (event->enc_change.status == 0 && ble_gap_conn_find(event->enc_change.conn_handle, &desc) == 0) {
            s_encrypted = desc.sec_state.encrypted;
            ESP_LOGI(TAG, "暗号化 %s（bonded=%d）", s_encrypted ? "OK" : "NG", desc.sec_state.bonded);
            s_peer_addr = desc.peer_id_addr;
            s_peer_addr_valid = true;
            if (!s_have_target) {
                print_addr("初回の相手（鍵の相互認証が通ったら登録します）:", &desc.peer_id_addr);
            }
            ble_gattc_disc_svc_by_uuid(s_conn, &UUID_SVC.u, on_svc, NULL);
        } else {
            ESP_LOGW(TAG, "暗号化失敗 status=%d（相手が鍵を失った可能性）", event->enc_change.status);
            if (ble_gap_conn_find(event->enc_change.conn_handle, &desc) == 0) {
                ble_store_util_delete_peer(&desc.peer_id_addr);
            }
            ble_gap_terminate(event->enc_change.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        return 0;

    case BLE_GAP_EVENT_NOTIFY_RX:
        /* ハンドシェイク中のHELLO/AUTHも受け取る（判定は on_packet）。
         * ボンディング済みの相手は購読状態が復元されるので、こちらがサービス探索を終える前に
         * 机上ユニットのHELLOが届くことがある → ハンドル未確定の間は（indication以外の）通知を受け入れる */
        if (s_encrypted && !event->notify_rx.indication &&
            (event->notify_rx.attr_handle == s_peer_to_recv || s_peer_to_recv == 0)) {
            push_rx(event->notify_rx.om);
        }
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU=%d", event->mtu.value);
        return 0;

    /* 机上ユニット（Peripheral）からの接続パラメータ変更要求は、そのまま受け入れる。
     * 0 を返すと NimBLE は相手の希望値で承認する（こちらから別の値を押し付けない） */
    case BLE_GAP_EVENT_CONN_UPDATE_REQ:
        ESP_LOGI(TAG, "机上ユニットから接続パラメータ変更要求: interval %u-%u(×1.25ms) latency %u timeout %u(×10ms) → 承認",
                 event->conn_update_req.peer_params->itvl_min, event->conn_update_req.peer_params->itvl_max,
                 event->conn_update_req.peer_params->latency, event->conn_update_req.peer_params->supervision_timeout);
        return 0;

    case BLE_GAP_EVENT_L2CAP_UPDATE_REQ:
        ESP_LOGI(TAG, "机上ユニットから接続パラメータ変更要求(L2CAP): interval %u-%u latency %u → 承認",
                 event->conn_update_req.peer_params->itvl_min, event->conn_update_req.peer_params->itvl_max,
                 event->conn_update_req.peer_params->latency);
        return 0;

    case BLE_GAP_EVENT_CONN_UPDATE:
        if (event->conn_update.status == 0) {
            log_conn_params("接続パラメータ更新:", event->conn_update.conn_handle);
        } else {
            ESP_LOGW(TAG, "接続パラメータ更新に失敗 status=%d", event->conn_update.status);
        }
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        s_conn_was_bonded = false;
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    return 0;
}

/* ======================================================================
 *  初期化
 * ====================================================================== */
static void on_reset(int reason)
{
    ESP_LOGE(TAG, "NimBLE reset reason=%d", reason);
}

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    uint8_t a[6] = { 0 };
    ble_hs_id_copy_addr(s_own_addr_type, a, NULL);
    ESP_LOGI(TAG, "自分のBLEアドレス %02x:%02x:%02x:%02x:%02x:%02x（登録済みペア数=%d）",
             a[5], a[4], a[3], a[2], a[1], a[0], bond_count());
    if (s_is_central) {
        central_scan();
    } else {
        periph_advertise();
    }
}

static void host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t ydl_ble_init(const yd_link_config_t *cfg)
{
    s_cfg = *cfg;
    s_is_central = (cfg->transport == YD_LINK_BLE_CENTRAL);
    s_cp_enabled = !s_is_central && cfg->ble_conn_power_save;

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs");

    s_sec_mutex = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_sec_mutex, ESP_ERR_NO_MEM, TAG, "mutex");
    const esp_timer_create_args_t ta = { .callback = hs_timeout_cb, .name = "yd_hs" };
    ESP_RETURN_ON_ERROR(esp_timer_create(&ta, &s_hs_timer), TAG, "timer");
    s_have_psk = load_psk();

    s_txq = xQueueCreate(TXQ_DEPTH, sizeof(pkt_t));
    s_rxq = xQueueCreate(RXQ_DEPTH, sizeof(pkt_t));
    ESP_RETURN_ON_FALSE(s_txq && s_rxq, ESP_ERR_NO_MEM, TAG, "queue");
    xTaskCreate(rx_task, "yd_ble_rx", 4096, NULL, 12, NULL);
    xTaskCreate(tx_task, "yd_ble_tx", 4096, NULL, 12, NULL);

    if (s_is_central) {
        target_load();
    }

    if (!s_have_psk) {
        /* 鍵がなければ電波も出さない（鍵なしで動く状態を作らない） */
        ESP_LOGE(TAG, "鍵がないためBLEを起動しません");
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(nimble_port_init(), TAG, "nimble init");

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = store_status_cb;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(s_is_central ? "KeyRelay-Recv" : DEVICE_NAME);
    ble_att_set_preferred_mtu(247);

    if (!s_is_central) {
        int rc = ble_gatts_count_cfg(s_svcs);
        ESP_RETURN_ON_FALSE(rc == 0, ESP_FAIL, TAG, "gatts count %d", rc);
        rc = ble_gatts_add_svcs(s_svcs);
        ESP_RETURN_ON_FALSE(rc == 0, ESP_FAIL, TAG, "gatts add %d", rc);
    }

    ble_store_config_init();
    nimble_port_freertos_init(host_task);
    ESP_LOGI(TAG, "BLE起動（%s）", s_is_central ? "レシーバー / Central" : "机上ユニット / Peripheral");
    return ESP_OK;
}
