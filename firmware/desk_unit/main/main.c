/*
 * KeyRelay 机上ユニット
 *
 *   YubiKey ──USB(ホスト)── ESP32-S3 ──BLE(またはUART)── レシーバー ──USB── Mac
 *
 * 1. USBホストでYubiKeyを列挙し、FIDO HIDインターフェース（usage page 0xF1D0）を掴む
 * 2. 接続のたびに CTAPHID_INIT を自分で投げて応答を確認する（セルフテスト）。
 *    応答に含まれる CTAPHID バージョン / デバイスFWバージョン / capabilities を覚えておく
 * 3. 64バイトのCTAPHIDレポートをレシーバーと相互中継する
 * 4. CCID インターフェース（PIV / OpenPGP / OATH）も掴み、CCID メッセージを丸ごと中継する（ccid_host.c）
 * 5. キーボード（OTP）インターフェースも掴み、タッチで打たれるキー入力を中継する（otp_host.c）
 *
 * ---- 省電力（v3、電池版のみ。USB給電版の既定では無効: YD_BATTERY_POWERED） ----
 *  YubiKey はバス稼働中 12.2mA、バス停止（USBサスペンド）中 0.3mA。そこで
 *  IDLE_SUSPEND_MS 無通信（かつ処理中のCTAPHIDトランザクションなし）でルートポートの
 *  電源を切ってバスを止める（VBUSはボードの5Vから出たままなので YubiKey はサスペンドに入る）。
 *  Mac から要求が届いたらキューに積んでバスを再開し、再列挙が終わってから順番に流す。
 *  意図的なサスペンドはレシーバーに「取り外し」と伝えない（MacからはFIDOキーが挿さったまま）。
 *
 * ---- CTAPHID チャンネルIDの仮想化 ----
 *  ESP-IDF v5.4 のUSBホストにはサスペンド/レジュームAPIがないため、「再開」は再列挙になり、
 *  YubiKey は Mac が持っているチャンネルID(CID)をすべて忘れてしまう。そこで
 *   - Mac のブロードキャストCIDへの CTAPHID_INIT は机上ユニットが自分で答え、仮想CIDを払い出す
 *     （応答のバージョン・capabilities は YubiKey の INIT 応答をキャッシュしたもの）
 *   - 仮想CIDごとに、最初の要求のときに YubiKey へ自前の CTAPHID_INIT を送って本物のCID
 *     （デバイスCID）を取得し、以後は全パケットの先頭4バイトを相互に書き換える
 *   - 再列挙のたびに epoch を進め、古いデバイスCIDはすべて無効にする（次の要求で取り直す）
 *  CTAPHID の他の中身（CBOR/U2F）は解釈しないので、YubiKey 4(U2F) でも 5(FIDO2) でも動く。
 *
 * ---- タスク構成 ----
 *  usb_client : in_cb（YubiKey→Mac。CID逆変換してそのまま送信）
 *  yd_link rx : link_rx（Mac→YubiKey。s_m2k_q に積むだけ）
 *  fido_tx    : s_m2k_q を順に処理（INIT代答 / 必要ならバス再開を待つ / CID変換して書き込み）
 *  app_main   : USBデバイスの接続・切断、サスペンド判定、バス再開、BOOTボタン、状態ログ
 *  CIDテーブルは s_lock（スピンロック、短時間のみ）、USBへの書き込みは s_bus_mutex で保護。
 */
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_pm.h"
#include "sdkconfig.h"
#include "usb/usb_host.h"
#include "usb/usb_helpers.h"
#include "driver/gpio.h"
#include "soc/uart_pins.h"
#include "soc/rtc_cntl_reg.h"
#include "driver/uart.h"
#include "esp_system.h"
#include "yd_link.h"
#include "ccid_host.h"
#include "otp_host.h"
#include "yd_lang.h"

static const char *TAG = "desk";

/* ---- 配線（README参照） ---- */
#define LINK_UART        1
#define LINK_TX_GPIO     17
#define LINK_RX_GPIO     18
#define LINK_BAUD        921600
/* 1にすると従来の有線UARTで中継（BLEの切り分け用。省電力のライトスリープは無効になる） */
#define YD_USE_UART      0

/* ---- 電源 ----
 * 0（既定）: USB給電。USBバスの自動停止をせず、BLEも常に低遅延の接続パラメータを使う。
 *            CCID（PIV/OpenPGP）はバスを止めるとカードがリセットされ PIN 入力済みの状態が消えるので、
 *            USB給電なら止めない方がよい
 * 1        : 電池駆動（v3 の省電力動作）。無通信 IDLE_SUSPEND_MS でバスを止める */
#define YD_BATTERY_POWERED   0

/* ---- ケースを開けずに書き換える（既定はオフ） ----
 * 1 にしてビルドすると、ログ用UART（CP2102N）に YD_DL_COMMAND の1行を送ったとき、ROMのダウンロードモードで再起動する。
 *    そのまま esptool / ESP Web Flasher で同じシリアルポートから書き込める（BOOTボタン不要）。
 * 注意: 机上ユニットの CP2102N を挿しているPCのソフトなら誰でも書き換え・読み出し（鍵を含む）が
 *       できるようになる。充電器につないで使うなら影響はない */
#define YD_UART_DOWNLOAD_CMD 0
#define YD_DL_COMMAND        "keyrelay-download"

/* ---- 省電力の設定 ---- */
#define IDLE_SUSPEND_MS      5000    /* この時間無通信ならUSBバスを止める */
#define RESUME_TIMEOUT_MS    3000    /* バス再開から FIDO インターフェース取得までの上限 */
#define RESUME_MIN_OFF_MS    300     /* 停止直後の再開は、USBホスト内部の後始末が終わるまで待つ */
#define MANUAL_HOLD_MS       30000   /* BOOT短押しで再開したときは、この間は自動サスペンドしない */
#define TXN_STALE_MS         15000   /* トランザクション中でも、これだけ無通信なら終わったとみなす */
#define STATUS_LOG_MS        10000   /* 状態サマリーのログ周期 */
#define PM_MAX_FREQ_MHZ      CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ
#define PM_MIN_FREQ_MHZ      40      /* XTAL */
#if CONFIG_PM_ENABLE && CONFIG_FREERTOS_USE_TICKLESS_IDLE && !YD_USE_UART
#define LIGHT_SLEEP_ON       1
#else
#define LIGHT_SLEEP_ON       0
#endif

/* ---- CTAPHID ---- */
#define FIDO_REPORT_LEN      64
#define CID_TABLE_SIZE       8       /* 仮想CIDの同時保持数（LRUで入れ替え） */
#define M2K_QUEUE_LEN        40      /* Mac→YubiKey の待ち行列（サスペンド中もここに溜まる） */
#define DEV_INIT_TIMEOUT_MS  500

#define CTAPHID_BROADCAST    0xFFFFFFFFu
#define CTAPHID_INIT         0x86
#define CTAPHID_CANCEL       0x91
#define CTAPHID_KEEPALIVE    0xBB
#define CTAPHID_ERROR        0xBF
#define CTAPHID_ERR_INVALID_CHANNEL 0x0B
#define CTAPHID_ERR_OTHER    0x7F
#define INIT_PKT_PAYLOAD     (FIDO_REPORT_LEN - 7)
#define CONT_PKT_PAYLOAD     (FIDO_REPORT_LEN - 5)

/* ---- アプリイベント ---- */
typedef enum {
    APP_EV_NEW_DEV,
    APP_EV_DEV_GONE,
    APP_EV_RESUME_REQ,     /* fido_tx: YubiKeyが必要なのでバスを起こしてほしい */
} app_ev_type_t;

typedef struct {
    app_ev_type_t type;
    uint8_t addr;
} app_ev_t;

typedef struct {
    uint8_t d[FIDO_REPORT_LEN];
} rep_t;

static QueueHandle_t s_app_q;
static QueueHandle_t s_m2k_q;
static usb_host_client_handle_t s_client;
static EventGroupHandle_t s_evg;
#define EVB_READY        BIT0     /* FIDOインターフェースを掴んで中継できる */
#define EVB_RESUME_FAIL  BIT1     /* 直近のバス再開が失敗した */

static SemaphoreHandle_t s_bus_mutex;   /* YubiKeyへの書き込み・open/close・サスペンドを直列化 */

static struct {
    usb_device_handle_t dev;
    int fido_itf;              /* -1 = 未検出 */
    uint8_t ep_in, ep_out;
    uint16_t mps;
    usb_transfer_t *xfer_in, *xfer_out, *xfer_ctrl;
    SemaphoreHandle_t out_done, ctrl_done;
    volatile bool in_busy;
    volatile bool gone;
    volatile bool relay_on;    /* trueでレシーバーへ中継、falseはセルフテスト中など */
} k = { .fido_itf = -1 };

/* ---- バス状態（app_main だけが書き換える） ---- */
typedef enum {
    BUS_ACTIVE,        /* ルートポート給電中（YubiKeyがいれば EVB_READY） */
    BUS_SUSPENDING,    /* 電源を切った。デバイスの後始末待ち */
    BUS_SUSPENDED,     /* 停止中（YubiKeyはUSBサスペンド） */
    BUS_RESUMING,      /* 再開済み、再列挙待ち */
} bus_state_t;

static volatile bus_state_t s_bus_state = BUS_ACTIVE;
static volatile bool s_key_present;     /* レシーバーに「接続済み」と伝えている状態か */
static volatile bool s_tx_busy;         /* fido_tx がパケット処理中 */

/* ---- CIDテーブル（s_lock で保護。中でログを出さないこと） ---- */
typedef struct {
    bool used;
    uint32_t vcid;         /* Mac が使う仮想CID */
    uint32_t dcid;         /* YubiKey 上の本物のCID（depoch が現在の epoch のときだけ有効） */
    uint32_t depoch;
    bool busy;             /* 要求を送ってから最終応答を返し終わるまで */
    bool ka_logged;
    uint32_t resp_rem;     /* 受信中の応答の残りバイト数 */
    int64_t last_us;
    uint32_t lru;
} cid_ent_t;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static cid_ent_t s_tab[CID_TABLE_SIZE];
static uint32_t s_epoch = 1;
static uint32_t s_lru_clock;
static uint32_t s_next_vcid;
static int64_t s_last_activity_us;

/* YubiKey の INIT 応答から覚えた値（Mac への INIT 代答に使う） */
static struct {
    bool valid;
    uint8_t proto, major, minor, build, caps;
} s_devinfo;

/* 自前の CTAPHID_INIT（セルフテスト / デバイスCID取得）の応答待ち */
static struct {
    volatile bool waiting;
    volatile bool ok;
    uint8_t nonce[8];
    volatile uint32_t cid;
    SemaphoreHandle_t done;
} s_alloc;

/* ---- 電源管理ロック ---- */
static esp_pm_lock_handle_t s_pm_nols, s_pm_apb;
static volatile bool s_pm_held;

/* ======================================================================
 *  小物
 * ====================================================================== */
static inline uint32_t get_cid(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static inline void put_cid(uint8_t *p, uint32_t cid)
{
    p[0] = cid >> 24; p[1] = cid >> 16; p[2] = cid >> 8; p[3] = cid;
}

static inline int64_t now_us(void)
{
    return esp_timer_get_time();
}

static inline bool is_ready(void)
{
    return (xEventGroupGetBits(s_evg) & EVB_READY) != 0;
}

static void note_activity(void)
{
    int64_t t = now_us();
    portENTER_CRITICAL(&s_lock);
    s_last_activity_us = t;
    portEXIT_CRITICAL(&s_lock);
}

/* USBホスト稼働中はライトスリープ禁止＋APBを最大に（USB OTG のクロックを止めない） */
static void pm_hold(bool on)
{
    if (s_pm_held == on) {
        return;
    }
    if (s_pm_nols) {
        on ? esp_pm_lock_acquire(s_pm_nols) : esp_pm_lock_release(s_pm_nols);
    }
    if (s_pm_apb) {
        on ? esp_pm_lock_acquire(s_pm_apb) : esp_pm_lock_release(s_pm_apb);
    }
    s_pm_held = on;
}

static const char *bus_state_name(bus_state_t st)
{
    switch (st) {
    case BUS_ACTIVE:     return "ON";
    case BUS_SUSPENDING: return YD_L("suspending", "停止処理中");
    case BUS_SUSPENDED:  return YD_L("suspended", "停止(サスペンド)");
    case BUS_RESUMING:   return YD_L("resuming", "再開中");
    }
    return "?";
}

/* ---- CIDテーブル操作（s_lock 保持中に呼ぶ） ---- */
static int tab_find_v(uint32_t vcid)
{
    for (int i = 0; i < CID_TABLE_SIZE; i++) {
        if (s_tab[i].used && s_tab[i].vcid == vcid) {
            return i;
        }
    }
    return -1;
}

static int tab_find_d(uint32_t dcid)
{
    for (int i = 0; i < CID_TABLE_SIZE; i++) {
        if (s_tab[i].used && s_tab[i].depoch == s_epoch && s_tab[i].dcid == dcid) {
            return i;
        }
    }
    return -1;
}

static bool dev_valid(const cid_ent_t *e)
{
    return e->depoch == s_epoch && e->dcid != 0;
}

/* 空き、なければ「処理中でない中で一番古い」、それもなければ一番古いエントリを返す */
static int tab_take_slot(uint32_t vcid, uint32_t *evicted)
{
    int best = -1;
    *evicted = 0;
    for (int i = 0; i < CID_TABLE_SIZE; i++) {
        if (!s_tab[i].used) {
            best = i;
            break;
        }
    }
    if (best < 0) {
        for (int pass = 0; pass < 2 && best < 0; pass++) {
            uint32_t oldest = UINT32_MAX;
            for (int i = 0; i < CID_TABLE_SIZE; i++) {
                if (pass == 0 && s_tab[i].busy) {
                    continue;
                }
                if (s_tab[i].lru < oldest) {
                    oldest = s_tab[i].lru;
                    best = i;
                }
            }
        }
        *evicted = s_tab[best].vcid;
    }
    memset(&s_tab[best], 0, sizeof(s_tab[best]));
    s_tab[best].used = true;
    s_tab[best].vcid = vcid;
    s_tab[best].lru = ++s_lru_clock;
    s_tab[best].last_us = now_us();
    return best;
}

static uint32_t new_vcid(void)
{
    for (;;) {
        uint32_t c = s_next_vcid++;
        if (c == 0 || c == CTAPHID_BROADCAST || tab_find_v(c) >= 0) {
            continue;
        }
        return c;
    }
}

/* 処理中のトランザクションがあるか（古すぎるものは終わったことにする） */
static bool any_busy_locked(int64_t t)
{
    bool busy = false;
    for (int i = 0; i < CID_TABLE_SIZE; i++) {
        if (s_tab[i].used && s_tab[i].busy) {
            if (t - s_tab[i].last_us > (int64_t)TXN_STALE_MS * 1000) {
                s_tab[i].busy = false;
            } else {
                busy = true;
            }
        }
    }
    return busy;
}

static int tab_count(void)
{
    int n = 0;
    portENTER_CRITICAL(&s_lock);
    for (int i = 0; i < CID_TABLE_SIZE; i++) {
        n += s_tab[i].used;
    }
    portEXIT_CRITICAL(&s_lock);
    return n;
}

/* ---- Mac へ返すパケット ---- */
static void send_to_mac(const uint8_t *rep)
{
    esp_err_t err = yd_link_send(YD_CH_FIDO, rep, FIDO_REPORT_LEN);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, YD_L("Send to the receiver failed: %s", "レシーバーへの送信失敗: %s"), esp_err_to_name(err));
    }
}

static void send_ctaphid_error(uint32_t cid, uint8_t code)
{
    uint8_t r[FIDO_REPORT_LEN] = { 0 };
    put_cid(r, cid);
    r[4] = CTAPHID_ERROR;
    r[6] = 1;
    r[7] = code;
    send_to_mac(r);
}

/* CTAPHID_INIT 応答（17バイト）を机上ユニットが組み立てて Mac に返す */
static void send_init_resp(uint32_t reply_cid, const uint8_t nonce[8], uint32_t new_cid)
{
    uint8_t r[FIDO_REPORT_LEN] = { 0 };
    put_cid(r, reply_cid);
    r[4] = CTAPHID_INIT;
    r[5] = 0;
    r[6] = 17;
    memcpy(&r[7], nonce, 8);
    put_cid(&r[15], new_cid);
    portENTER_CRITICAL(&s_lock);
    bool valid = s_devinfo.valid;
    r[19] = valid ? s_devinfo.proto : 2;
    r[20] = valid ? s_devinfo.major : 0;
    r[21] = valid ? s_devinfo.minor : 0;
    r[22] = valid ? s_devinfo.build : 0;
    r[23] = valid ? s_devinfo.caps : 0x05;   /* 不明時: WINK + CBOR */
    portEXIT_CRITICAL(&s_lock);
    if (!valid) {
        ESP_LOGW(TAG, YD_L("Key information not available yet: answering INIT with defaults (CTAPHID v2, caps=0x05)", "YubiKeyの情報が未取得のため、既定値(CTAPHID v2, caps=0x05)でINITに応答します"));
    }
    send_to_mac(r);
}

/* ======================================================================
 *  USB Host ライブラリ / クライアント タスク
 * ====================================================================== */
static void usb_lib_task(void *arg)
{
    for (;;) {
        uint32_t flags;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
    }
}

static void client_event_cb(const usb_host_client_event_msg_t *msg, void *arg)
{
    app_ev_t ev = { 0 };
    if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        ev.type = APP_EV_NEW_DEV;
        ev.addr = msg->new_dev.address;
    } else {
        ev.type = APP_EV_DEV_GONE;
        k.gone = true;
    }
    xQueueSend(s_app_q, &ev, 0);
}

static void client_task(void *arg)
{
    usb_host_client_config_t cfg = {
        .is_synchronous = false,
        .max_num_event_msg = 5,
        .async = { .client_event_callback = client_event_cb, .callback_arg = NULL },
    };
    ESP_ERROR_CHECK(usb_host_client_register(&cfg, &s_client));
    xTaskNotifyGive((TaskHandle_t)arg);
    for (;;) {
        usb_host_client_handle_events(s_client, portMAX_DELAY);
    }
}

/* ======================================================================
 *  YubiKey → Mac（usb_client タスクのコンテキストで呼ばれる）
 * ====================================================================== */

/* ブロードキャストCIDの応答 = 机上ユニット自身の INIT への応答。Macには流さない */
static void handle_broadcast_resp(const uint8_t *r)
{
    uint16_t bcnt = (r[5] << 8) | r[6];
    if (s_alloc.waiting && r[4] == CTAPHID_INIT && bcnt >= 17 && memcmp(&r[7], s_alloc.nonce, 8) == 0) {
        portENTER_CRITICAL(&s_lock);
        s_devinfo.proto = r[19];
        s_devinfo.major = r[20];
        s_devinfo.minor = r[21];
        s_devinfo.build = r[22];
        s_devinfo.caps = r[23];
        s_devinfo.valid = true;
        portEXIT_CRITICAL(&s_lock);
        s_alloc.cid = get_cid(&r[15]);
        s_alloc.ok = true;
        s_alloc.waiting = false;
        xSemaphoreGive(s_alloc.done);
        return;
    }
    if (s_alloc.waiting && r[4] == CTAPHID_ERROR) {
        ESP_LOGW(TAG, YD_L("Key answered INIT with an error code=0x%02x", "YubiKeyがINITにエラー応答 code=0x%02x"), r[7]);
        s_alloc.ok = false;
        s_alloc.waiting = false;
        xSemaphoreGive(s_alloc.done);
        return;
    }
    ESP_LOGW(TAG, YD_L("Dropping unexpected broadcast CID response CMD=0x%02x", "ブロードキャストCIDの想定外の応答を破棄 CMD=0x%02x"), r[4]);
}

static void relay_key_to_mac(const uint8_t *in)
{
    uint8_t r[FIDO_REPORT_LEN];
    memcpy(r, in, FIDO_REPORT_LEN);
    uint32_t dcid = get_cid(r);
    bool init = (r[4] & 0x80) != 0;
    uint8_t cmd = r[4];
    uint16_t bcnt = (r[5] << 8) | r[6];
    int64_t t = now_us();

    bool found = false, final = false, log_ka = false, invalidated = false, cid_changed = false;
    uint32_t vcid = 0;
    portENTER_CRITICAL(&s_lock);
    int i = tab_find_d(dcid);
    if (i >= 0) {
        cid_ent_t *e = &s_tab[i];
        found = true;
        vcid = e->vcid;
        e->last_us = t;
        s_last_activity_us = t;
        if (init) {
            if (cmd == CTAPHID_KEEPALIVE) {
                log_ka = !e->ka_logged;
                e->ka_logged = true;
            } else {
                e->resp_rem = bcnt > INIT_PKT_PAYLOAD ? bcnt - INIT_PKT_PAYLOAD : 0;
                if (e->resp_rem == 0) {
                    e->busy = false;
                    final = true;
                }
                if (cmd == CTAPHID_INIT && bcnt >= 17) {
                    /* チャンネル上の INIT（再同期）への応答: 本文中のCIDも仮想CIDに書き換える */
                    uint32_t nd = get_cid(&r[15]);
                    if (nd != 0 && nd != CTAPHID_BROADCAST && nd != e->dcid) {
                        e->dcid = nd;
                        cid_changed = true;
                    }
                    put_cid(&r[15], vcid);
                }
                if (cmd == CTAPHID_ERROR && bcnt >= 1 && r[7] == CTAPHID_ERR_INVALID_CHANNEL) {
                    e->depoch = 0;      /* 次の要求でデバイスCIDを取り直す */
                    invalidated = true;
                }
            }
        } else if (e->resp_rem > 0) {
            e->resp_rem = e->resp_rem > CONT_PKT_PAYLOAD ? e->resp_rem - CONT_PKT_PAYLOAD : 0;
            if (e->resp_rem == 0) {
                e->busy = false;
                final = true;
            }
        }
    }
    portEXIT_CRITICAL(&s_lock);

    if (!found) {
        if (init) {
            ESP_LOGW(TAG, YD_L("Dropping response with no matching virtual CID devCID=%08", "対応する仮想CIDがない応答を破棄 devCID=%08") PRIx32 " CMD=0x%02x", dcid, cmd);
        }
        return;
    }
    if (init && cmd != CTAPHID_KEEPALIVE) {
        ESP_LOGI(TAG, "Key→Mac devCID=%08" PRIx32 "→vCID=%08" PRIx32 " CMD=0x%02x len=%u%s",
                 dcid, vcid, cmd, bcnt, final ? YD_L(" (done)", "（完了）") : "");
    } else if (log_ka) {
        ESP_LOGI(TAG, "Key→Mac vCID=%08" PRIx32 YD_L(" KEEPALIVE status=%u (waiting for touch etc.; the bus stays up)", " KEEPALIVE status=%u（タッチ待ちなど。バスは止めません）"), vcid, r[7]);
    } else if (final) {
        ESP_LOGI(TAG, "Key→Mac vCID=%08" PRIx32 YD_L(" response sent", " 応答の送信完了"), vcid);
    }
    if (invalidated) {
        ESP_LOGW(TAG, YD_L("Key reported an invalid CID. Device CID for vCID=%08", "YubiKeyがCIDを無効と応答。vCID=%08") PRIx32 YD_L(" will be re-acquired next time", " のデバイスCIDを次回取り直します"), vcid);
    }
    if (cid_changed) {
        ESP_LOGI(TAG, YD_L("Key CID changed after resync vCID=%08", "再同期でYubiKeyのCIDが変わりました vCID=%08") PRIx32, vcid);
    }
    put_cid(r, vcid);
    send_to_mac(r);
}

static void in_cb(usb_transfer_t *x)
{
    k.in_busy = false;
    if (x->status == USB_TRANSFER_STATUS_COMPLETED && x->actual_num_bytes > 0) {
        const uint8_t *r = x->data_buffer;
        if (get_cid(r) == CTAPHID_BROADCAST) {
            handle_broadcast_resp(r);
        } else if (k.relay_on) {
            relay_key_to_mac(r);
        }
    } else if (x->status != USB_TRANSFER_STATUS_COMPLETED) {
        ESP_LOGD(TAG, "IN status=%d", x->status);
    }
    if (!k.gone && x->status == USB_TRANSFER_STATUS_COMPLETED) {
        x->num_bytes = k.mps;
        if (usb_host_transfer_submit(x) == ESP_OK) {
            k.in_busy = true;
        }
    }
}

static void out_cb(usb_transfer_t *x)
{
    if (x->status != USB_TRANSFER_STATUS_COMPLETED) {
        ESP_LOGW(TAG, "OUT status=%d", x->status);
    }
    xSemaphoreGive(k.out_done);
}

static void ctrl_cb(usb_transfer_t *x)
{
    xSemaphoreGive(k.ctrl_done);
}

/* ======================================================================
 *  YubiKey への書き込み（s_bus_mutex を持って呼ぶ）
 * ====================================================================== */
static esp_err_t fido_write(const uint8_t *report)
{
    if (k.fido_itf < 0 || k.gone || !k.dev) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(k.out_done, 0);   /* 古い完了通知を捨てる */
    memcpy(k.xfer_out->data_buffer, report, FIDO_REPORT_LEN);
    k.xfer_out->num_bytes = FIDO_REPORT_LEN;
    esp_err_t err = usb_host_transfer_submit(k.xfer_out);
    if (err != ESP_OK) {
        return err;
    }
    if (xSemaphoreTake(k.out_done, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "OUT timeout");
        return ESP_ERR_TIMEOUT;
    }
    return k.xfer_out->status == USB_TRANSFER_STATUS_COMPLETED ? ESP_OK : ESP_FAIL;
}

/* YubiKey に自前の CTAPHID_INIT（ブロードキャスト、乱数nonce）を送り、新しいCIDをもらう。
 * 応答は handle_broadcast_resp() が受け取り、Mac には流れない。s_bus_mutex を持って呼ぶ */
static esp_err_t dev_init_channel(uint32_t *out_cid)
{
    xSemaphoreTake(s_alloc.done, 0);   /* 遅れて届いた古い通知を捨てる */
    uint8_t rep[FIDO_REPORT_LEN] = { 0xFF, 0xFF, 0xFF, 0xFF, CTAPHID_INIT, 0x00, 0x08 };
    esp_fill_random(s_alloc.nonce, sizeof(s_alloc.nonce));
    memcpy(&rep[7], s_alloc.nonce, 8);
    s_alloc.ok = false;
    s_alloc.waiting = true;           /* 応答は書き込み完了より先に来ることがあるので先に立てる */
    esp_err_t err = fido_write(rep);
    if (err != ESP_OK) {
        s_alloc.waiting = false;
        return err;
    }
    if (xSemaphoreTake(s_alloc.done, pdMS_TO_TICKS(DEV_INIT_TIMEOUT_MS)) != pdTRUE) {
        s_alloc.waiting = false;
        return ESP_ERR_TIMEOUT;
    }
    if (!s_alloc.ok) {
        return ESP_FAIL;
    }
    *out_cid = s_alloc.cid;
    return ESP_OK;
}

/* ======================================================================
 *  Mac → YubiKey（fido_tx タスク）
 * ====================================================================== */

/* YubiKey が使える状態になるまで待つ（必要なら app_main にバス再開を頼む） */
static bool ensure_ready(void)
{
    if (is_ready()) {
        return true;
    }
    xEventGroupClearBits(s_evg, EVB_RESUME_FAIL);
    app_ev_t ev = { .type = APP_EV_RESUME_REQ };
    xQueueSend(s_app_q, &ev, pdMS_TO_TICKS(100));
    EventBits_t b = xEventGroupWaitBits(s_evg, EVB_READY | EVB_RESUME_FAIL, pdFALSE, pdFALSE,
                                        pdMS_TO_TICKS(RESUME_TIMEOUT_MS + RESUME_MIN_OFF_MS + 1000));
    return (b & EVB_READY) != 0;
}

/* s_bus_mutex を持ち、EVB_READY の状態で呼ぶ */
static void forward_locked(uint8_t *pkt, uint32_t vcid, bool init, uint8_t cmd)
{
    uint32_t dcid = 0, evicted = 0;
    bool valid = false, readopted = false;
    portENTER_CRITICAL(&s_lock);
    int i = tab_find_v(vcid);
    if (i < 0) {
        i = tab_take_slot(vcid, &evicted);
        readopted = true;
    }
    valid = dev_valid(&s_tab[i]);
    dcid = s_tab[i].dcid;
    uint32_t epoch = s_epoch;
    portEXIT_CRITICAL(&s_lock);
    if (readopted) {
        ESP_LOGW(TAG, "vCID=%08" PRIx32 YD_L(" re-registered in the table (evicted: %08", " をテーブルに再登録（追い出し: %08") PRIx32 YD_L(")", "）"), vcid, evicted);
    }

    if (!valid) {
        if (!init) {
            ESP_LOGW(TAG, YD_L("Dropping continuation packet without a device CID vCID=%08", "継続パケットだがデバイスCIDが未割当のため破棄 vCID=%08") PRIx32, vcid);
            return;
        }
        int64_t t0 = now_us();
        esp_err_t err = dev_init_channel(&dcid);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, YD_L("Failed to get a CID from the key (%s). Returning an error to the host vCID=%08", "YubiKeyのCID取得に失敗 (%s)。Macにエラーを返します vCID=%08") PRIx32, esp_err_to_name(err), vcid);
            send_ctaphid_error(vcid, CTAPHID_ERR_OTHER);
            return;
        }
        portENTER_CRITICAL(&s_lock);
        i = tab_find_v(vcid);
        if (i >= 0) {
            s_tab[i].dcid = dcid;
            s_tab[i].depoch = s_epoch;
        }
        portEXIT_CRITICAL(&s_lock);
        ESP_LOGI(TAG, YD_L("CID mapping: vCID=%08", "CID対応付け: vCID=%08") PRIx32 " ⇔ devCID=%08" PRIx32 YD_L(" (epoch %", "（epoch %") PRIu32 YD_L(", %lldms)", ", %lldms）"),
                 vcid, dcid, epoch, (now_us() - t0) / 1000);
    }

    put_cid(pkt, dcid);
    bool txn = init && cmd != CTAPHID_CANCEL;   /* CANCEL 自体には応答がない */
    if (txn) {
        portENTER_CRITICAL(&s_lock);
        i = tab_find_v(vcid);
        if (i >= 0) {
            s_tab[i].busy = true;
            s_tab[i].ka_logged = false;
            s_tab[i].resp_rem = 0;
            s_tab[i].last_us = now_us();
            s_tab[i].lru = ++s_lru_clock;
        }
        portEXIT_CRITICAL(&s_lock);
    }
    esp_err_t err = fido_write(pkt);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, YD_L("Write to the key failed: %s", "YubiKeyへの書き込み失敗: %s"), esp_err_to_name(err));
        if (txn) {
            portENTER_CRITICAL(&s_lock);
            i = tab_find_v(vcid);
            if (i >= 0) {
                s_tab[i].busy = false;
            }
            portEXIT_CRITICAL(&s_lock);
            send_ctaphid_error(vcid, CTAPHID_ERR_OTHER);
        }
    }
}

static void handle_mac_packet(uint8_t *pkt)
{
    uint32_t cid = get_cid(pkt);
    bool init = (pkt[4] & 0x80) != 0;
    uint8_t cmd = pkt[4];
    uint16_t bcnt = (pkt[5] << 8) | pkt[6];

    if (init) {
        ESP_LOGI(TAG, "Mac→Key vCID=%08" PRIx32 " CMD=0x%02x len=%u%s", cid, cmd, bcnt,
                 is_ready() ? "" : YD_L(" (bus suspended)", "（バス停止中）"));
    }
    if (cid == 0) {
        ESP_LOGW(TAG, YD_L("Dropping packet with CID=0", "CID=0 のパケットを破棄"));
        return;
    }

    /* ---- ブロードキャストCID: INIT は机上ユニットが代答（YubiKeyは起こさない） ---- */
    if (cid == CTAPHID_BROADCAST) {
        if (!init || cmd != CTAPHID_INIT) {
            if (init) {
                send_ctaphid_error(CTAPHID_BROADCAST, CTAPHID_ERR_INVALID_CHANNEL);
            }
            return;
        }
        bool have_info;
        portENTER_CRITICAL(&s_lock);
        have_info = s_devinfo.valid;
        portEXIT_CRITICAL(&s_lock);
        if (!have_info) {
            ESP_LOGI(TAG, YD_L("Key information not available yet: waking the bus to read it", "YubiKeyの情報が未取得なので、一度バスを起こして取得します"));
            ensure_ready();   /* 接続時のセルフテストで s_devinfo が埋まる */
        }
        uint32_t vcid, evicted;
        portENTER_CRITICAL(&s_lock);
        vcid = new_vcid();
        tab_take_slot(vcid, &evicted);
        portEXIT_CRITICAL(&s_lock);
        send_init_resp(CTAPHID_BROADCAST, &pkt[7], vcid);
        if (evicted) {
            ESP_LOGI(TAG, YD_L("Answered the host's INIT: assigned vCID=%08", "MacのINITに代答: 仮想CID=%08") PRIx32 YD_L(" (evicted old vCID=%08", " を割当（古いvCID=%08") PRIx32 YD_L(")", " を追い出し）"), vcid, evicted);
        } else {
            ESP_LOGI(TAG, YD_L("Answered the host's INIT: assigned vCID=%08", "MacのINITに代答: 仮想CID=%08") PRIx32 YD_L("", " を割当"), vcid);
        }
        return;
    }

    /* ---- チャンネルCID ---- */
    uint32_t evicted = 0;
    bool adopted = false, busy, valid;
    portENTER_CRITICAL(&s_lock);
    int i = tab_find_v(cid);
    if (i < 0) {
        /* 机上ユニット再起動前に払い出したCIDなど。そのまま仮想CIDとして引き継ぐ */
        i = tab_take_slot(cid, &evicted);
        adopted = true;
    }
    s_tab[i].lru = ++s_lru_clock;
    busy = s_tab[i].busy;
    valid = dev_valid(&s_tab[i]);
    portEXIT_CRITICAL(&s_lock);
    if (adopted) {
        ESP_LOGI(TAG, YD_L("Registered unknown CID=%08", "未知のCID=%08") PRIx32 YD_L(" as a virtual CID%s", " を仮想CIDとして登録%s"), cid, evicted ? YD_L(" (evicted one LRU entry)", "（LRUで1件追い出し）") : "");
    }

    if (init && cmd == CTAPHID_INIT) {
        /* チャンネル上の INIT（再同期）: YubiKey側で処理中なら本物に転送して中断させる。
         * それ以外は同じCIDで代答するだけ（YubiKeyは起こさない） */
        if (!(busy && valid && is_ready())) {
            portENTER_CRITICAL(&s_lock);
            i = tab_find_v(cid);
            if (i >= 0) {
                s_tab[i].busy = false;
            }
            portEXIT_CRITICAL(&s_lock);
            send_init_resp(cid, &pkt[7], cid);
            ESP_LOGI(TAG, YD_L("Answered channel resync INIT vCID=%08", "チャンネル再同期INITに代答 vCID=%08") PRIx32, cid);
            return;
        }
        ESP_LOGI(TAG, YD_L("Resync INIT on a busy channel → forwarding to the key vCID=%08", "処理中のチャンネルへの再同期INIT → YubiKeyへ転送 vCID=%08") PRIx32, cid);
    }
    if (init && cmd == CTAPHID_CANCEL && !(busy && valid)) {
        /* YubiKey側に中断すべき処理がない（応答もない）ので、起こさずに捨てる */
        ESP_LOGI(TAG, YD_L("No request in progress: dropping CANCEL vCID=%08", "処理中の要求がないので CANCEL を破棄 vCID=%08") PRIx32, cid);
        return;
    }

    /* ---- YubiKey へ転送（必要ならバスを起こす） ---- */
    for (int attempt = 0; attempt < 2; attempt++) {
        if (!is_ready()) {
            if (!init) {
                ESP_LOGW(TAG, YD_L("Key not ready: dropping continuation packet vCID=%08", "YubiKey未準備のため継続パケットを破棄 vCID=%08") PRIx32, cid);
                return;
            }
            int64_t t0 = now_us();
            if (!ensure_ready()) {
                break;
            }
            ESP_LOGI(TAG, YD_L("Key ready (waited %lldms). Sending queued requests", "YubiKey準備完了（待ち %lldms）。キューの要求を流します"), (now_us() - t0) / 1000);
        }
        xSemaphoreTake(s_bus_mutex, portMAX_DELAY);
        if (is_ready()) {
            forward_locked(pkt, cid, init, cmd);
            xSemaphoreGive(s_bus_mutex);
            return;
        }
        xSemaphoreGive(s_bus_mutex);   /* 直前にサスペンドされた → もう一度起こす */
    }
    ESP_LOGW(TAG, YD_L("Key unavailable: dropping request vCID=%08", "YubiKeyが使えないため要求を破棄 vCID=%08") PRIx32 " CMD=0x%02x", cid, cmd);
    if (init && cmd != CTAPHID_CANCEL) {
        send_ctaphid_error(cid, CTAPHID_ERR_OTHER);
    }
}

static void fido_tx_task(void *arg)
{
    rep_t p;
    for (;;) {
        xQueueReceive(s_m2k_q, &p, portMAX_DELAY);
        s_tx_busy = true;
        handle_mac_packet(p.d);
        note_activity();
        s_tx_busy = false;
    }
}

/* ======================================================================
 *  リンク（BLE/UART）
 * ====================================================================== */
/* YubiKey の状態をレシーバーへ: [ATTACHED, 使える機能] / [DETACHED] */
static bool s_has_fido;   /* 直近の列挙で FIDO を掴めたか（意図的なサスペンド中も保持） */

static void report_key_state(void)
{
    if (s_key_present) {
        uint8_t m[2] = { YD_CTRL_KEY_ATTACHED, (s_has_fido ? YD_KEY_CAP_FIDO : 0) | (ccidh_present() ? YD_KEY_CAP_CCID : 0) |
                                               (otph_present() ? YD_KEY_CAP_OTP | YD_KEY_CAP_OTPCTL : 0) };
        yd_link_send(YD_CH_CTRL, m, sizeof(m));
    } else {
        yd_link_send_ctrl(YD_CTRL_KEY_DETACHED);
    }
}

static void link_state(bool up, void *arg)
{
    if (up) {
        ESP_LOGI(TAG, YD_L("Connected to the receiver over BLE", "レシーバーとBLEで接続しました"));
        report_key_state();
    } else {
        ESP_LOGI(TAG, YD_L("BLE connection to the receiver lost (waiting for reconnection)", "レシーバーとのBLE接続が切れました（再接続を待ちます）"));
    }
}

/* レシーバーから届いたフレーム（yd_link受信タスク）。キューに積むだけ */
static void link_rx(uint8_t ch, const uint8_t *data, uint16_t len, void *arg)
{
    if (ch == YD_CH_CCID) {
        ccidh_from_link(data, len);
    } else if (ch == YD_CH_OTPCTL) {
        otph_ctl_from_link(data, len);
    } else if (ch == YD_CH_FIDO && len == FIDO_REPORT_LEN) {
        rep_t p;
        memcpy(p.d, data, FIDO_REPORT_LEN);
        note_activity();
        if (xQueueSend(s_m2k_q, &p, 0) != pdTRUE) {
            ESP_LOGW(TAG, YD_L("Host→key queue overflow: packet dropped CMD=0x%02x", "Mac→Key キュー溢れ。パケットを破棄 CMD=0x%02x"), data[4]);
        }
    } else if (ch == YD_CH_CTRL && len >= 1 && data[0] == YD_CTRL_HELLO) {
        ESP_LOGI(TAG, YD_L("Receiver connection confirmed", "レシーバー接続を確認"));
        /* レシーバーが後から起動した場合に状態を伝え直す。意図的なサスペンド中も「接続済み」 */
        report_key_state();
    }
}

/* ======================================================================
 *  列挙とインターフェース探索（app_main から、s_bus_mutex を持って呼ぶ）
 * ====================================================================== */
static const char *itf_class_name(uint8_t cls, uint8_t proto)
{
    switch (cls) {
    case 0x03: return proto == 1 ? YD_L("HID (keyboard = OTP)", "HID(キーボード=OTP)") : "HID";
    case 0x0B: return YD_L("CCID (smart card = PIV/OpenPGP)", "CCID(スマートカード=PIV/OpenPGP)");
    default:   return YD_L("other", "その他");
    }
}

/* HIDレポートディスクリプタを取得して FIDO usage page (06 D0 F1) を探す */
static bool hid_is_fido(uint8_t itf, uint16_t rd_len, bool verbose)
{
    if (rd_len == 0 || rd_len > 248) {
        rd_len = 248;
    }
    usb_setup_packet_t *s = (usb_setup_packet_t *)k.xfer_ctrl->data_buffer;
    s->bmRequestType = USB_BM_REQUEST_TYPE_DIR_IN | USB_BM_REQUEST_TYPE_TYPE_STANDARD | USB_BM_REQUEST_TYPE_RECIP_INTERFACE;
    s->bRequest = USB_B_REQUEST_GET_DESCRIPTOR;
    s->wValue = (0x22 << 8);   /* Report descriptor */
    s->wIndex = itf;
    s->wLength = rd_len;
    k.xfer_ctrl->num_bytes = sizeof(usb_setup_packet_t) + rd_len;
    k.xfer_ctrl->device_handle = k.dev;
    k.xfer_ctrl->bEndpointAddress = 0;
    k.xfer_ctrl->callback = ctrl_cb;
    xSemaphoreTake(k.ctrl_done, 0);
    if (usb_host_transfer_submit_control(s_client, k.xfer_ctrl) != ESP_OK) {
        return false;
    }
    if (xSemaphoreTake(k.ctrl_done, pdMS_TO_TICKS(1000)) != pdTRUE ||
        k.xfer_ctrl->status != USB_TRANSFER_STATUS_COMPLETED) {
        ESP_LOGW(TAG, YD_L("itf%u: failed to read the report descriptor", "itf%u: レポートディスクリプタ取得失敗"), itf);
        return false;
    }
    const uint8_t *rd = k.xfer_ctrl->data_buffer + sizeof(usb_setup_packet_t);
    int n = k.xfer_ctrl->actual_num_bytes - (int)sizeof(usb_setup_packet_t);
    if (verbose) {
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, rd, n > 32 ? 32 : n, ESP_LOG_INFO);
    }
    for (int i = 0; i + 2 < n; i++) {
        if (rd[i] == 0x06 && rd[i + 1] == 0xD0 && rd[i + 2] == 0xF1) {
            return true;
        }
    }
    return false;
}

/* 戻り値: FIDOインターフェースを掴んで中継を始められたら true */
static bool open_device(uint8_t addr)
{
    /* 復帰（再列挙）のたびにディスクリプタを全部ダンプするとUARTログだけで数百msかかるので、
     * 詳細ログは初回 / 本当の抜き挿しのあとだけ */
    bool verbose = !s_key_present;
    if (k.dev) {
        ESP_LOGW(TAG, YD_L("Previous device not closed yet: ignoring the new device", "前のデバイスが閉じられていません。新しいデバイスは無視します"));
        return false;
    }
    if (usb_host_device_open(s_client, addr, &k.dev) != ESP_OK) {
        ESP_LOGE(TAG, YD_L("device open failed addr=%u", "device open失敗 addr=%u"), addr);
        k.dev = NULL;
        return false;
    }
    k.gone = false;
    k.fido_itf = -1;

    portENTER_CRITICAL(&s_lock);
    s_epoch++;            /* 再列挙 → YubiKey は以前のCIDをすべて忘れている */
    portEXIT_CRITICAL(&s_lock);

    const usb_device_desc_t *dd;
    const usb_config_desc_t *cd;
    usb_host_get_device_descriptor(k.dev, &dd);
    usb_host_get_active_config_descriptor(k.dev, &cd);
    if (verbose) {
        ESP_LOGI(TAG, YD_L("==== USB device attached VID=%04x PID=%04x ====", "==== USBデバイス接続 VID=%04x PID=%04x ===="), dd->idVendor, dd->idProduct);
        usb_print_device_descriptor(dd);
        usb_print_config_descriptor(cd, NULL);
    } else {
        ESP_LOGI(TAG, YD_L("Key re-enumerated VID=%04x PID=%04x", "YubiKey再認識 VID=%04x PID=%04x"), dd->idVendor, dd->idProduct);
    }

    /* インターフェースを列挙 */
    const usb_intf_desc_t *ccid_itf = NULL, *kbd_itf = NULL;
    int ccid_off = 0, kbd_off = 0;
    int offset = 0;
    const usb_standard_desc_t *d = (const usb_standard_desc_t *)cd;
    while ((d = usb_parse_next_descriptor_of_type(d, cd->wTotalLength, USB_B_DESCRIPTOR_TYPE_INTERFACE, &offset)) != NULL) {
        const usb_intf_desc_t *itf = (const usb_intf_desc_t *)d;
        if (itf->bAlternateSetting != 0) {
            continue;
        }
        if (verbose) {
            ESP_LOGI(TAG, YD_L("itf%u: class=0x%02x proto=%u endpoints=%u → %s", "itf%u: class=0x%02x proto=%u EP数=%u → %s"),
                     itf->bInterfaceNumber, itf->bInterfaceClass, itf->bInterfaceProtocol,
                     itf->bNumEndpoints, itf_class_name(itf->bInterfaceClass, itf->bInterfaceProtocol));
        }

        if (itf->bInterfaceClass == 0x0B && !ccid_itf) {
            ccid_itf = itf;
            ccid_off = offset;
            continue;
        }
        if (itf->bInterfaceClass == 0x03 && itf->bInterfaceSubClass == 1 && itf->bInterfaceProtocol == 1 && !kbd_itf) {
            kbd_itf = itf;      /* ブートキーボード = OTP */
            kbd_off = offset;
            continue;
        }
        if (itf->bInterfaceClass != 0x03 || itf->bNumEndpoints != 2 || k.fido_itf >= 0) {
            continue;
        }
        /* HIDディスクリプタ(0x21)からレポートディスクリプタ長を取る */
        uint16_t rd_len = 0;
        int o2 = offset;
        const usb_standard_desc_t *h = usb_parse_next_descriptor(d, cd->wTotalLength, &o2);
        if (h && h->bDescriptorType == 0x21 && h->bLength >= 9) {
            const uint8_t *hb = (const uint8_t *)h;
            rd_len = hb[7] | (hb[8] << 8);
        }
        if (!hid_is_fido(itf->bInterfaceNumber, rd_len, verbose)) {
            continue;
        }
        /* エンドポイント取得 */
        for (int e = 0; e < 2; e++) {
            int o3 = offset;
            const usb_ep_desc_t *ep = usb_parse_endpoint_descriptor_by_index(itf, e, cd->wTotalLength, &o3);
            if (!ep) {
                continue;
            }
            if (ep->bEndpointAddress & 0x80) {
                k.ep_in = ep->bEndpointAddress;
                k.mps = USB_EP_DESC_GET_MPS(ep);
            } else {
                k.ep_out = ep->bEndpointAddress;
            }
        }
        k.fido_itf = itf->bInterfaceNumber;
        if (verbose) {
            ESP_LOGI(TAG, YD_L("→ Found FIDO HID: itf%u EP IN=0x%02x OUT=0x%02x MPS=%u", "→ FIDO HID を発見: itf%u EP IN=0x%02x OUT=0x%02x MPS=%u"),
                     k.fido_itf, k.ep_in, k.ep_out, k.mps);
        }
    }

    esp_err_t err;
    if (k.fido_itf < 0) {
        ESP_LOGW(TAG, YD_L("No FIDO interface (check that FIDO is enabled with `ykman config usb`)", "FIDOインターフェースが見つかりません（`ykman config usb` でFIDOが有効か確認）"));
    } else if ((err = usb_host_interface_claim(s_client, k.dev, k.fido_itf, 0)) != ESP_OK) {
        ESP_LOGE(TAG, YD_L("Failed to claim the FIDO interface: %s", "FIDOインターフェースの取得に失敗: %s"), esp_err_to_name(err));
        k.fido_itf = -1;
    }
    bool ccid_ok = false;
    if (ccid_itf) {
        ccid_ok = ccidh_attach(s_client, k.dev, cd, ccid_itf, ccid_off, verbose);
    } else if (verbose) {
        ESP_LOGW(TAG, YD_L("No CCID interface (check that PIV/OpenPGP/OATH are enabled with `ykman config usb`)", "CCIDインターフェースがありません（`ykman config usb` でPIV/OpenPGP/OATHが有効か確認）"));
    }
    bool kbd_ok = false;
    if (kbd_itf) {
        kbd_ok = otph_attach(s_client, k.dev, cd, kbd_itf, kbd_off, verbose);
    } else if (verbose) {
        ESP_LOGW(TAG, YD_L("No keyboard (OTP) interface (check that OTP is enabled with `ykman config usb`)", "キーボード（OTP）インターフェースがありません（`ykman config usb` でOTPが有効か確認）"));
    }
    if (k.fido_itf < 0) {
        if (!ccid_ok && !kbd_ok) {
            return false;
        }
        /* FIDO なし・CCID のみ */
        s_has_fido = false;
        bool was = s_key_present;
        s_key_present = true;
        report_key_state();
        if (!was) {
            ESP_LOGI(TAG, YD_L("Relay started (no FIDO)", "中継開始（FIDOなし）"));
        }
        return true;
    }

    k.xfer_in->device_handle = k.dev;
    k.xfer_in->bEndpointAddress = k.ep_in;
    k.xfer_in->callback = in_cb;
    k.xfer_in->num_bytes = k.mps;
    k.xfer_out->device_handle = k.dev;
    k.xfer_out->bEndpointAddress = k.ep_out;
    k.xfer_out->callback = out_cb;

    /* IN を常時ポーリング開始 */
    k.relay_on = false;
    if (usb_host_transfer_submit(k.xfer_in) == ESP_OK) {
        k.in_busy = true;
    }

    /* セルフテスト: CTAPHID_INIT をブロードキャストCIDで送る（応答は s_devinfo にキャッシュ）。
     * 復帰時も毎回行う（数ms）。Macからの要求は EVB_READY が立つまで fido_tx で待たされるので混ざらない */
    uint32_t tcid = 0;
    err = dev_init_channel(&tcid);
    if (err == ESP_OK) {
        if (verbose) {
            uint8_t caps = s_devinfo.caps;
            ESP_LOGI(TAG, YD_L("Self-test OK: CTAPHID v%u, device FW %u.%u.%u, caps=0x%02x [%s%s%s]", "セルフテストOK: CTAPHID v%u, デバイスFW %u.%u.%u, caps=0x%02x [%s%s%s]"),
                     s_devinfo.proto, s_devinfo.major, s_devinfo.minor, s_devinfo.build, caps,
                     (caps & 0x01) ? "WINK " : "",
                     (caps & 0x04) ? "CBOR(FIDO2) " : YD_L("U2F-only ", "U2Fのみ "),
                     (caps & 0x08) ? "NMSG" : "");
        } else {
            ESP_LOGI(TAG, YD_L("Self-test OK (epoch %", "セルフテストOK（epoch %") PRIu32 YD_L(")", "）"), s_epoch);
        }
    } else {
        ESP_LOGW(TAG, YD_L("No self-test response (%s). Starting the relay anyway", "セルフテスト応答なし（%s）。中継は開始します"), esp_err_to_name(err));
    }

    k.relay_on = true;
    note_activity();
    xEventGroupSetBits(s_evg, EVB_READY);
    s_has_fido = true;
    bool was = s_key_present;
    s_key_present = true;
    report_key_state();   /* 復帰（再列挙）のときも送る: CCID が戻ったことをレシーバーに伝える */
    if (!was) {
        ESP_LOGI(TAG, YD_L("Relay started. Try webauthn.io in a browser, `gpg --card-status`, etc.", "中継開始。Macのブラウザで webauthn.io、`gpg --card-status` などを試してください"));
    }
    return true;
}

static void close_device(void)
{
    if (!k.dev) {
        return;
    }
    bool deliberate = (s_bus_state == BUS_SUSPENDING);
    xEventGroupClearBits(s_evg, EVB_READY);
    k.relay_on = false;
    ccidh_detach();
    otph_detach();
    if (k.fido_itf >= 0) {
        usb_host_endpoint_halt(k.dev, k.ep_in);
        usb_host_endpoint_flush(k.dev, k.ep_in);
        for (int i = 0; i < 100 && k.in_busy; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        usb_host_endpoint_clear(k.dev, k.ep_in);
        usb_host_interface_release(s_client, k.dev, k.fido_itf);
    }
    usb_host_device_close(s_client, k.dev);
    k.dev = NULL;
    k.fido_itf = -1;

    /* デバイスCIDはすべて無効。処理中だったトランザクションを集める */
    uint32_t dead[CID_TABLE_SIZE];
    int ndead = 0;
    portENTER_CRITICAL(&s_lock);
    s_epoch++;
    for (int i = 0; i < CID_TABLE_SIZE; i++) {
        if (s_tab[i].used && s_tab[i].busy) {
            s_tab[i].busy = false;
            dead[ndead++] = s_tab[i].vcid;
        }
    }
    portEXIT_CRITICAL(&s_lock);

    if (deliberate) {
        ESP_LOGI(TAG, YD_L("Key suspended (intentional, so the receiver is not told the key was removed; CCID is unavailable while suspended)", "YubiKeyをサスペンドしました（意図的な停止なので、レシーバーには取り外しを通知しません。CCIDは使えなくなります）"));
        report_key_state();
        return;
    }
    ESP_LOGI(TAG, YD_L("==== USB device removed ====", "==== USBデバイス取り外し ===="));
    if (s_key_present) {
        s_key_present = false;
        s_has_fido = false;
        report_key_state();
    }
    for (int i = 0; i < ndead; i++) {
        ESP_LOGW(TAG, YD_L("Returning an error to in-flight vCID=%08", "処理中だった vCID=%08") PRIx32 YD_L("", " にエラーを返します"), dead[i]);
        send_ctaphid_error(dead[i], CTAPHID_ERR_OTHER);
    }
}

/* ======================================================================
 *  バスの停止 / 再開（app_main だけが呼ぶ）
 * ====================================================================== */
static int64_t s_suspend_t0, s_resume_t0, s_resume_deadline, s_manual_hold_until;
static bool s_resume_pending;

static void bus_suspend(const char *why, bool require_idle)
{
    if (s_bus_state != BUS_ACTIVE || !is_ready()) {
        return;
    }
    if (xSemaphoreTake(s_bus_mutex, 0) != pdTRUE) {
        return;   /* fido_tx が書き込み中。次の周期でまた判定する */
    }
    /* 判定してからミューテックスを取るまでの間に要求が転送されていないか、取ってから確認し直す */
    int64_t t = now_us();
    portENTER_CRITICAL(&s_lock);
    bool busy = any_busy_locked(t);
    int64_t idle = t - s_last_activity_us;
    portEXIT_CRITICAL(&s_lock);
    if (busy || uxQueueMessagesWaiting(s_m2k_q) > 0 ||
        (require_idle && idle < (int64_t)IDLE_SUSPEND_MS * 1000)) {
        xSemaphoreGive(s_bus_mutex);
        return;
    }
    s_bus_state = BUS_SUSPENDING;
    xEventGroupClearBits(s_evg, EVB_READY);
    k.relay_on = false;
    xSemaphoreGive(s_bus_mutex);

    esp_err_t err = usb_host_lib_set_root_port_power(false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, YD_L("USB bus suspend failed: %s", "USBバス停止に失敗: %s"), esp_err_to_name(err));
        s_bus_state = BUS_ACTIVE;
        if (k.dev && k.fido_itf >= 0) {
            k.relay_on = true;
            xEventGroupSetBits(s_evg, EVB_READY);
        }
        return;
    }
    s_suspend_t0 = now_us();
    ESP_LOGI(TAG, YD_L("■ USB bus suspended (%s). The key enters suspend", "■ USBバス停止（%s）。YubiKeyはサスペンドに入ります"), why);
    /* 実際のデバイス解放は DEV_GONE → close_device()。BUS_SUSPENDED への遷移は app_main のループで */
}

static void bus_resume(const char *why)
{
    if (s_bus_state == BUS_SUSPENDING) {
        s_resume_pending = true;   /* 停止処理が終わったら再開する */
        return;
    }
    if (s_bus_state != BUS_SUSPENDED) {
        return;
    }
    /* 停止直後は USB ホスト内部でポートの後始末（recover）が走るので少し待つ */
    int64_t since_off = (now_us() - s_suspend_t0) / 1000;
    if (since_off < RESUME_MIN_OFF_MS) {
        vTaskDelay(pdMS_TO_TICKS(RESUME_MIN_OFF_MS - since_off));
    }
    pm_hold(true);
    esp_err_t err = ESP_FAIL;
    for (int i = 0; i < 25; i++) {
        err = usb_host_lib_set_root_port_power(true);
        if (err == ESP_OK) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, YD_L("USB bus resume failed: %s", "USBバス再開に失敗: %s"), esp_err_to_name(err));
        pm_hold(false);
        xEventGroupSetBits(s_evg, EVB_RESUME_FAIL);
        return;
    }
    s_bus_state = BUS_RESUMING;
    s_resume_t0 = now_us();
    s_resume_deadline = s_resume_t0 + (int64_t)RESUME_TIMEOUT_MS * 1000;
    ESP_LOGI(TAG, YD_L("▶ USB bus resumed (%s). Waiting for the key to re-enumerate", "▶ USBバス再開（%s）。YubiKeyの再認識を待ちます"), why);
}

static void handle_resume_req(void)
{
    if (is_ready()) {
        return;
    }
    switch (s_bus_state) {
    case BUS_SUSPENDED:
    case BUS_SUSPENDING:
        bus_resume(YD_L("host request", "Macからの要求"));
        break;
    case BUS_RESUMING:
        break;   /* 再列挙待ち中 */
    case BUS_ACTIVE:
        /* 給電中なのに YubiKey がいない（抜かれている / 列挙中）。少しだけ待ってみる */
        s_bus_state = BUS_RESUMING;
        s_resume_t0 = now_us();
        s_resume_deadline = s_resume_t0 + (int64_t)RESUME_TIMEOUT_MS * 1000;
        ESP_LOGI(TAG, YD_L("Key not visible: waiting %d s", "YubiKeyが見えないので %d 秒待ちます"), RESUME_TIMEOUT_MS / 1000);
        break;
    }
}

/* ======================================================================
 *  電源管理
 * ====================================================================== */
static void pm_init(void)
{
#if CONFIG_PM_ENABLE
    esp_pm_config_t pc = {
        .max_freq_mhz = PM_MAX_FREQ_MHZ,
        .min_freq_mhz = PM_MIN_FREQ_MHZ,
        .light_sleep_enable = LIGHT_SLEEP_ON,
    };
    esp_err_t err = esp_pm_configure(&pc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, YD_L("esp_pm_configure failed: %s", "esp_pm_configure 失敗: %s"), esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, YD_L("Power management: CPU %d-%dMHz, automatic light sleep=%s", "電源管理: CPU %d〜%dMHz, 自動ライトスリープ=%s"), PM_MIN_FREQ_MHZ, PM_MAX_FREQ_MHZ,
                 LIGHT_SLEEP_ON ? YD_L("on (only while the USB bus is suspended)", "有効（USBバス停止中のみ）") : YD_L("off", "無効"));
    }
    if (esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "usb_host", &s_pm_nols) != ESP_OK) {
        s_pm_nols = NULL;
    }
    if (esp_pm_lock_create(ESP_PM_APB_FREQ_MAX, 0, "usb_host_apb", &s_pm_apb) != ESP_OK) {
        s_pm_apb = NULL;
    }
#if YD_USE_UART
    /* 有線UARTリンクはライトスリープ中に受信できず、APB変化でボーレートもずれるので常に起きている */
    static esp_pm_lock_handle_t s_uart_nols, s_uart_apb;
    if (esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "uart_link", &s_uart_nols) == ESP_OK) {
        esp_pm_lock_acquire(s_uart_nols);
    }
    if (esp_pm_lock_create(ESP_PM_APB_FREQ_MAX, 0, "uart_link_apb", &s_uart_apb) == ESP_OK) {
        esp_pm_lock_acquire(s_uart_apb);
    }
#endif
#if LIGHT_SLEEP_ON
    /* ライトスリープ中は全GPIOがアイソレートされる（ESP_SLEEP_GPIO_RESET_WORKAROUND）。
     *  - BOOTボタン(GPIO0): プルアップを保つ
     *  - USB D-/D+(GPIO19/20): 起きているときと同じ状態を保つ（サスペンド中のYubiKeyを乱さない）
     *  - コンソールUART0 TX/RX: TXが浮いてログが化けないように */
    gpio_sleep_sel_dis(GPIO_NUM_0);
    gpio_sleep_sel_dis(GPIO_NUM_19);
    gpio_sleep_sel_dis(GPIO_NUM_20);
    gpio_sleep_sel_dis(U0TXD_GPIO_NUM);
    gpio_sleep_sel_dis(U0RXD_GPIO_NUM);
#endif
#else
    ESP_LOGW(TAG, YD_L("CONFIG_PM_ENABLE is off: no power saving (DFS / light sleep)", "CONFIG_PM_ENABLE が無効のため省電力（DFS/ライトスリープ）は使いません"));
#endif
}

/* ======================================================================
 *  状態ログ
 * ====================================================================== */
static void log_status(void)
{
    yd_link_conn_info_t ci;
    yd_link_get_conn_info(&ci);
    int64_t t = now_us();
    portENTER_CRITICAL(&s_lock);
    int64_t idle_ms = (t - s_last_activity_us) / 1000;
    uint32_t epoch = s_epoch;
    portEXIT_CRITICAL(&s_lock);
    unsigned itvl_x100 = (unsigned)ci.itvl * 125;
#if YD_USE_UART
    const char *link = "UART";
#else
    const char *link = ci.connected ? YD_L("connected", "接続中") : YD_L("disconnected", "未接続");
#endif
    ESP_LOGI(TAG, YD_L("[status] link=%s bus=%s%s conn=%u.%02ums/lat%u/%ums(%s%s) PM lock=%s vCID=%d/%d epoch=%", "[状態] リンク=%s バス=%s%s 接続パラメータ=%u.%02ums/lat%u/%ums(%s%s) PMロック=%s vCID=%d/%d epoch=%") PRIu32 YD_L(" last traffic %llds ago", " 最終通信から%llds"),
             link, bus_state_name(s_bus_state), is_ready() ? YD_L("(relaying)", "(中継可)") : "",
             itvl_x100 / 100, itvl_x100 % 100, ci.latency, (unsigned)ci.timeout * 10,
             ci.power_save ? YD_L("power save", "省電力") : YD_L("low latency", "低遅延"), ci.update_pending ? YD_L(",updating", ",更新中") : "",
             s_pm_held ? YD_L("held (no sleep)", "保持(スリープ禁止)") : YD_L("released (light sleep allowed)", "解放(ライトスリープ可)"),
             tab_count(), CID_TABLE_SIZE, epoch, idle_ms / 1000);
}

/* ======================================================================
 *  ログ用UARTからのコマンド
 * ====================================================================== */
#if YD_UART_DOWNLOAD_CMD
static void console_task(void *arg)
{
    /* コンソールUART0の受信だけドライバーで読む（ログの送信はこれまでどおり） */
    if (uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0) != ESP_OK) {
        ESP_LOGW(TAG, YD_L("Could not start console UART input", "コンソールUARTの受信を開始できませんでした"));
        vTaskDelete(NULL);
    }
    char line[48];
    size_t n = 0;
    for (;;) {
        uint8_t ch;
        if (uart_read_bytes(UART_NUM_0, &ch, 1, portMAX_DELAY) != 1) {
            continue;
        }
        if (ch == '\r' || ch == '\n') {
            line[n] = 0;
            if (n > 0 && strcmp(line, YD_DL_COMMAND) == 0) {
                ESP_LOGW(TAG, YD_L("Rebooting into download mode. Flash through this serial port (replug the power afterwards)", "ダウンロードモードで再起動します。このシリアルポートから書き込んでください（終わったら電源を挿し直す）"));
                vTaskDelay(pdMS_TO_TICKS(200));
                REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
                esp_restart();
            } else if (n > 0) {
                ESP_LOGI(TAG, YD_L("Command: %s → unknown (only \"%s\" is supported)", "コマンド: %s → 不明（使えるのは \"%s\" だけです）"), line, YD_DL_COMMAND);
            }
            n = 0;
        } else if (n < sizeof(line) - 1) {
            line[n++] = (char)ch;
        }
    }
}
#endif

/* ======================================================================
 *  app_main
 * ====================================================================== */
void app_main(void)
{
    ESP_LOGI(TAG, YD_L("KeyRelay desk unit starting", "KeyRelay 机上ユニット 起動"));
    s_app_q = xQueueCreate(8, sizeof(app_ev_t));
    s_m2k_q = xQueueCreate(M2K_QUEUE_LEN, sizeof(rep_t));
    s_evg = xEventGroupCreate();
    s_bus_mutex = xSemaphoreCreateMutex();
    s_alloc.done = xSemaphoreCreateBinary();
    k.out_done = xSemaphoreCreateBinary();
    k.ctrl_done = xSemaphoreCreateBinary();
    s_next_vcid = (esp_random() & 0x0FFFFFFF) | 0x10000000;

    /* 電源管理は BLE 初期化より前に設定する（ESP-IDF の power_save 例と同じ順序） */
    pm_init();
    pm_hold(true);   /* USBホスト開始時はルートポートが給電されるので */

    yd_link_config_t lc = {
#if YD_USE_UART
        .transport = YD_LINK_UART,
        .uart_num = LINK_UART, .tx_gpio = LINK_TX_GPIO, .rx_gpio = LINK_RX_GPIO, .baudrate = LINK_BAUD,
#else
        .transport = YD_LINK_BLE_PERIPHERAL,
        .ble_conn_power_save = YD_BATTERY_POWERED,
#endif
        .rx_cb = link_rx, .rx_cb_arg = NULL,
        .state_cb = link_state, .state_cb_arg = NULL,
    };
    ESP_ERROR_CHECK(yd_link_init(&lc));
    yd_link_send_ctrl(YD_CTRL_HELLO);   /* UART時のみ届く（BLEは接続後にレシーバーからHELLOが来る） */

    usb_host_config_t hc = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    ESP_ERROR_CHECK(usb_host_install(&hc));
    xTaskCreatePinnedToCore(usb_lib_task, "usb_lib", 4096, NULL, 10, NULL, 0);
    xTaskCreatePinnedToCore(client_task, "usb_client", 4096, xTaskGetCurrentTaskHandle(), 9, NULL, 0);
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    ESP_ERROR_CHECK(usb_host_transfer_alloc(FIDO_REPORT_LEN, 0, &k.xfer_in));
    ESP_ERROR_CHECK(usb_host_transfer_alloc(FIDO_REPORT_LEN, 0, &k.xfer_out));
    ESP_ERROR_CHECK(usb_host_transfer_alloc(sizeof(usb_setup_packet_t) + 248, 0, &k.xfer_ctrl));

    xTaskCreatePinnedToCore(fido_tx_task, "fido_tx", 4096, NULL, 8, NULL, 0);
    ccidh_init();
    otph_init();
#if YD_UART_DOWNLOAD_CMD
    xTaskCreate(console_task, "console", 3072, NULL, 2, NULL);
    ESP_LOGI(TAG, YD_L("To reflash without opening the case, send the line \"%s\" on this serial port", "ケースを開けずに書き換えるには、このシリアルに \"%s\" と1行送ってください"), YD_DL_COMMAND);
#endif

    ESP_LOGI(TAG, YD_L("Plug in the key (USB-A breakout)", "YubiKeyを挿してください（USB-A基板）"));
#if YD_BATTERY_POWERED
    ESP_LOGI(TAG, YD_L("The USB bus suspends after %d s without traffic. BOOT short press: suspend/resume (no auto-suspend for %d s after resume), hold 3 s: erase pairing", "無通信 %d 秒でUSBバスを自動停止します。BOOT短押し: 停止/再開（再開後%d秒は自動停止しない）、3秒長押し: ペアリング消去"),
             IDLE_SUSPEND_MS / 1000, MANUAL_HOLD_MS / 1000);
#else
    ESP_LOGI(TAG, YD_L("USB-powered mode: the USB bus never auto-suspends. BOOT short press: suspend/resume, hold 3 s: erase pairing", "USB給電モード: USBバスは自動停止しません。BOOT短押し: 停止/再開、3秒長押し: ペアリング消去"));
#endif

    gpio_config_t btn = {
        .pin_bit_mask = 1ULL << GPIO_NUM_0,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&btn);
    int prev_level = 1;
    TickType_t pressed_at = 0;
    TickType_t last_stats = xTaskGetTickCount();
    TickType_t last_status = xTaskGetTickCount();

    for (;;) {
        app_ev_t ev;
        if (xQueueReceive(s_app_q, &ev, pdMS_TO_TICKS(50)) == pdTRUE) {
            switch (ev.type) {
            case APP_EV_NEW_DEV: {
                xSemaphoreTake(s_bus_mutex, portMAX_DELAY);
                bool ok = open_device(ev.addr);
                xSemaphoreGive(s_bus_mutex);
                if (ok && s_bus_state == BUS_RESUMING) {
                    ESP_LOGI(TAG, YD_L("▶ Resume complete (%lldms after bus resume)", "▶ 復帰完了（バス再開から %lldms）"), (now_us() - s_resume_t0) / 1000);
                    s_bus_state = BUS_ACTIVE;
                } else if (ok && s_bus_state == BUS_SUSPENDED) {
                    s_bus_state = BUS_ACTIVE;   /* 念のため */
                }
                break;
            }
            case APP_EV_DEV_GONE:
                xSemaphoreTake(s_bus_mutex, portMAX_DELAY);
                close_device();
                xSemaphoreGive(s_bus_mutex);
                break;
            case APP_EV_RESUME_REQ:
                handle_resume_req();
                break;
            }
        }
        int64_t t = now_us();

        /* ---- 停止処理の完了待ち ---- */
        if (s_bus_state == BUS_SUSPENDING) {
            if (k.dev && (t - s_suspend_t0) > 1000 * 1000) {
                ESP_LOGW(TAG, YD_L("No DEV_GONE event: closing the device ourselves", "DEV_GONE が来ないので自分でデバイスを閉じます"));
                xSemaphoreTake(s_bus_mutex, portMAX_DELAY);
                close_device();
                xSemaphoreGive(s_bus_mutex);
            }
            if (!k.dev) {
                s_bus_state = BUS_SUSPENDED;
                pm_hold(false);
                ESP_LOGI(TAG, YD_L("■ Suspend complete. PM lock released (light sleep allowed)", "■ サスペンド完了。PMロック解放（ライトスリープ可）"));
                if (s_resume_pending || uxQueueMessagesWaiting(s_m2k_q) > 0 || s_tx_busy) {
                    s_resume_pending = false;
                    bus_resume(YD_L("request while suspended", "停止中に要求あり"));
                }
            }
        }

        /* ---- 再開のタイムアウト ---- */
        if (s_bus_state == BUS_RESUMING && !is_ready() && t > s_resume_deadline) {
            ESP_LOGE(TAG, YD_L("The key did not re-enumerate within %d s (it may have been removed)", "YubiKeyが %d 秒以内に再認識されませんでした（抜かれた可能性）"), RESUME_TIMEOUT_MS / 1000);
            s_bus_state = BUS_ACTIVE;   /* 給電したまま、挿し直しを待つ */
            xEventGroupSetBits(s_evg, EVB_RESUME_FAIL);
            if (s_key_present) {
                s_key_present = false;
                s_has_fido = false;
                report_key_state();
            }
        }

        /* ---- 無通信による自動サスペンド（電池版のみ） ---- */
        if (YD_BATTERY_POWERED && s_bus_state == BUS_ACTIVE && is_ready() && !s_tx_busy &&
            uxQueueMessagesWaiting(s_m2k_q) == 0 && t >= s_manual_hold_until) {
            portENTER_CRITICAL(&s_lock);
            bool busy = any_busy_locked(t);
            int64_t idle = t - s_last_activity_us;
            portEXIT_CRITICAL(&s_lock);
            int64_t ccid_idle = t - ccidh_last_activity_us();
            if (!busy && idle >= (int64_t)IDLE_SUSPEND_MS * 1000 && ccid_idle >= (int64_t)IDLE_SUSPEND_MS * 1000) {
                bus_suspend(YD_L("idle", "無通信"), true);
            }
        }

        /* ---- BOOTボタン（押すとLOW） ----
         *   短押し      : USBバス停止 / 再開（電流測定用）
         *   3秒以上長押し: BLEのペアリング情報を消去 */
        int level = gpio_get_level(GPIO_NUM_0);
        if (prev_level == 1 && level == 0) {
            pressed_at = xTaskGetTickCount();
        }
        if (prev_level == 0 && level == 1) {
            TickType_t held = xTaskGetTickCount() - pressed_at;
            if (held >= pdMS_TO_TICKS(3000)) {
                yd_link_forget_peers();
                ESP_LOGW(TAG, YD_L("Pairing data erased. Erase the receiver's pairing too, then restart both units", "ペアリング情報を消去しました。レシーバー側も消去してから、両方を再起動してください"));
            } else if (s_bus_state == BUS_ACTIVE && is_ready()) {
                s_manual_hold_until = 0;
                bus_suspend(YD_L("BOOT button", "BOOTボタン"), false);
                if (s_bus_state == BUS_SUSPENDING) {
                    ESP_LOGI(TAG, YD_L("[current measurement] the current shortly after this is the suspend current", "[電流測定] まもなくの電流値が『サスペンド電流』です"));
                } else {
                    ESP_LOGW(TAG, YD_L("Busy, could not suspend. Press again", "通信中のため停止できませんでした。もう一度押してください"));
                }
            } else if (s_bus_state == BUS_SUSPENDED) {
                s_manual_hold_until = now_us() + (int64_t)MANUAL_HOLD_MS * 1000;
                bus_resume(YD_L("BOOT button", "BOOTボタン"));
                ESP_LOGI(TAG, YD_L("[current measurement] the bus stays up for %d s (measure the active current)", "[電流測定] %d 秒間はバスを止めません（稼働電流を測れます）"), MANUAL_HOLD_MS / 1000);
            } else {
                ESP_LOGW(TAG, YD_L("Button ignored in bus state=%s", "バス状態=%s のためボタン操作を無視しました"), bus_state_name(s_bus_state));
            }
            vTaskDelay(pdMS_TO_TICKS(100));   /* チャタリング防止 */
        }
        prev_level = level;

        if ((xTaskGetTickCount() - last_stats) >= pdMS_TO_TICKS(3000)) {
            last_stats = xTaskGetTickCount();
            yd_link_stats_t st;
            yd_link_get_stats(&st);
            static uint32_t last_rx = UINT32_MAX, last_err = UINT32_MAX;
            if (st.rx_frames != last_rx || st.rx_crc_errors != last_err) {
                ESP_LOGI(TAG, YD_L("link stats: received from receiver=%", "link統計: レシーバーから受信=%") PRIu32 YD_L(" CRC errors=%", " CRCエラー=%") PRIu32 YD_L(" sent=%", " 送信=%") PRIu32 YD_L(" send drops=%", " 送信破棄=%") PRIu32,
                         st.rx_frames, st.rx_crc_errors, st.tx_frames, st.tx_dropped);
                last_rx = st.rx_frames; last_err = st.rx_crc_errors;
            }
        }
        if ((xTaskGetTickCount() - last_status) >= pdMS_TO_TICKS(STATUS_LOG_MS)) {
            last_status = xTaskGetTickCount();
            log_status();
        }
    }
}
