/*
 * ccid_host: YubiKey(CCID) ⇔ レシーバー
 *
 *   レシーバー ──yd_frag──▶ [組み立て] ──▶ ccid タスク ──bulk OUT──▶ YubiKey
 *   レシーバー ◀──yd_frag── ccid タスク ◀──[組み立て]◀──bulk IN(64バイトずつ)── YubiKey
 *
 * bulk IN は「1パケット = 1転送」で常に2本出しておき、ヘッダーの dwLength でメッセージに組み立てる
 * （64の倍数ぴったりのメッセージで長さ0のパケットが来るかどうかに依存しないため）。
 * 割り込みエンドポイント（カード挿抜通知）は YubiKey では意味がないので読まない。
 * カードの有無はレシーバーが「YubiKey の有無＋リンク状態」から自分で Mac に通知する。
 *
 * カード（PIV / OpenPGP）の状態は、PIN を確かめた接続のもの。レシーバーとつなぎ直したら、新しい接続の要求を
 * 通す前に自分で PowerOff / PowerOn を送ってカードをリセットする（レシーバーが PowerOn を省いても PIN を
 * 確かめ直させる）。できなければ、バスを止めて再列挙させるまで「カードなし」で答える。前の接続の要求、
 * 組み立て途中の断片、前の接続の要求への応答は、新しい接続に流さない。
 */
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/message_buffer.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "yd_link.h"
#include "yd_frag.h"
#include "yd_ccid.h"
#include "ccid_host.h"
#include "yd_lang.h"

static const char *TAG = "ccid";

#define IN_XFERS        2
#define OUT_TIMEOUT_MS  2000
#define POWER_TIMEOUT_MS 3000       /* カードのリセット（PowerOff / PowerOn それぞれ）の応答を待つ上限 */
#define CONN_TAG        4           /* 要求の先頭に付ける、届いた接続の番号（yd_link_conn_id） */
#define MB_SIZE         (2 * (YD_CCID_BUF + CONN_TAG + 8))

static struct {
    usb_host_client_handle_t client;
    usb_device_handle_t dev;
    int itf;
    uint8_t ep_in, ep_out;
    uint16_t mps;
    usb_transfer_t *xin[IN_XFERS];
    usb_transfer_t *xout;
    SemaphoreHandle_t out_done;
    SemaphoreHandle_t mutex;          /* OUT 書き込みと detach を直列化 */
    volatile int in_flight;
    volatile bool attached;
} c = { .itf = -1 };

/* YubiKey → 組み立て（usb_client タスク） */
static uint8_t s_up_asm[YD_CCID_BUF];
static size_t s_up_len;
static bool s_up_discard;

/* レシーバー → 組み立て（yd_link 受信タスク）。先頭 CONN_TAG バイトは接続の番号を入れる場所 */
static uint8_t s_down_asm[CONN_TAG + YD_CCID_BUF];
static yd_frag_rx_t s_down_rx;
static uint32_t s_down_conn;          /* s_down_rx で組み立てている要求の接続 */

/* ccid タスク */
static uint8_t s_req[CONN_TAG + YD_CCID_BUF], s_rsp[YD_CCID_BUF];
static MessageBufferHandle_t s_down_mb, s_up_mb;
static TaskHandle_t s_task;
static volatile int64_t s_last_us;
static struct {
    bool active;
    uint8_t type, seq;
    uint32_t conn;                    /* どの接続の要求か（応答はその接続にだけ返す） */
    int64_t t0;
} s_pend;

/* カードの状態がどの接続のものか（ccid タスク）。s_card_fresh と s_blocked は ccidh_attach でも書く */
static volatile bool s_card_fresh;    /* 列挙し直したばかりで、まだどの接続も使っていない */
static volatile bool s_blocked;       /* リセットできなかった。再列挙まで「カードなし」で答える */
static uint32_t s_card_conn;
static uint8_t s_own_seq;
static void (*s_bus_reset)(void);

static inline void kick(void)
{
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

static const char *type_name(uint8_t t)
{
    switch (t) {
    case CCID_PC_POWER_ON:   return "PowerOn";
    case CCID_PC_POWER_OFF:  return "PowerOff";
    case CCID_PC_GET_SLOT:   return "GetSlotStatus";
    case CCID_PC_XFR_BLOCK:  return "XfrBlock";
    case CCID_PC_GET_PARAMS: return "GetParameters";
    case CCID_PC_SET_PARAMS: return "SetParameters";
    case CCID_PC_ESCAPE:     return "Escape";
    case CCID_PC_ABORT:      return "Abort";
    default:                 return "?";
    }
}

/* ======================================================================
 *  YubiKey → （usb_client タスクのコンテキスト）
 * ====================================================================== */
static void up_drop(const char *why)
{
    ESP_LOGW(TAG, YD_L("Dropping response from the key: %s", "YubiKeyからの応答を破棄: %s"), why);
    s_up_len = 0;
}

static void up_packet(const uint8_t *p, size_t n)
{
    if (s_up_discard) {
        if (n < c.mps) {
            s_up_discard = false;
        }
        return;
    }
    if (n == 0) {
        if (s_up_len) {
            up_drop(YD_L("too short", "長さが足りない"));
        }
        return;
    }
    if (s_up_len + n > sizeof(s_up_asm)) {
        up_drop(YD_L("too long", "長すぎる"));
        s_up_discard = (n == c.mps);
        return;
    }
    memcpy(s_up_asm + s_up_len, p, n);
    s_up_len += n;
    if (s_up_len >= YD_CCID_HDR) {
        uint32_t dl = yd_ccid_dwlen(s_up_asm);
        if (dl > sizeof(s_up_asm) - YD_CCID_HDR) {
            up_drop(YD_L("dwLength over the limit", "dwLength が上限超え"));
            s_up_discard = (n == c.mps);
            return;
        }
        size_t total = YD_CCID_HDR + dl;
        if (s_up_len >= total) {
            if (xMessageBufferSend(s_up_mb, s_up_asm, total, 0) != total) {
                ESP_LOGW(TAG, YD_L("Response queue full: dropped", "応答キューが一杯のため破棄"));
            }
            s_up_len = 0;
            kick();
            return;
        }
    }
    if (n < c.mps) {
        up_drop(YD_L("truncated", "途中で終わった"));
    }
}

static void in_cb(usb_transfer_t *x)
{
    if (x->status == USB_TRANSFER_STATUS_COMPLETED) {
        up_packet(x->data_buffer, x->actual_num_bytes);
        if (c.attached) {
            x->num_bytes = c.mps;
            if (usb_host_transfer_submit(x) == ESP_OK) {
                return;   /* in_flight はそのまま */
            }
        }
    } else if (x->status != USB_TRANSFER_STATUS_CANCELED && x->status != USB_TRANSFER_STATUS_NO_DEVICE) {
        ESP_LOGW(TAG, "bulk IN status=%d", x->status);
    }
    c.in_flight--;
}

static void out_cb(usb_transfer_t *x)
{
    xSemaphoreGive(c.out_done);
}

/* ======================================================================
 *  接続 / 切断
 * ====================================================================== */
bool ccidh_attach(usb_host_client_handle_t client, usb_device_handle_t dev, const usb_config_desc_t *cd,
                  const usb_intf_desc_t *itf, int itf_offset, bool verbose)
{
    if (c.attached) {
        return true;
    }
    uint8_t ep_in = 0, ep_out = 0;
    uint16_t mps = 0;
    for (int e = 0; e < itf->bNumEndpoints; e++) {
        int o = itf_offset;
        const usb_ep_desc_t *ep = usb_parse_endpoint_descriptor_by_index(itf, e, cd->wTotalLength, &o);
        if (!ep || USB_EP_DESC_GET_XFERTYPE(ep) != USB_TRANSFER_TYPE_BULK) {
            continue;
        }
        if (ep->bEndpointAddress & 0x80) {
            ep_in = ep->bEndpointAddress;
            mps = USB_EP_DESC_GET_MPS(ep);
        } else {
            ep_out = ep->bEndpointAddress;
        }
    }
    if (!ep_in || !ep_out || mps == 0 || mps > 64) {
        ESP_LOGW(TAG, YD_L("CCID bulk endpoints not found", "CCIDのbulkエンドポイントが見つかりません"));
        return false;
    }

    /* CCID 機能ディスクリプタ（0x21）。レシーバーは YubiKey 5 と同じ値を Mac に名乗るので、違えば警告 */
    int o = itf_offset;
    const usb_standard_desc_t *fd = usb_parse_next_descriptor((const usb_standard_desc_t *)itf, cd->wTotalLength, &o);
    if (fd && fd->bDescriptorType == 0x21 && fd->bLength >= 54) {
        const uint8_t *b = (const uint8_t *)fd;
        uint32_t protocols = b[6] | (b[7] << 8) | (b[8] << 16) | ((uint32_t)b[9] << 24);
        uint32_t features = b[40] | (b[41] << 8) | (b[42] << 16) | ((uint32_t)b[43] << 24);
        uint32_t maxmsg = b[44] | (b[45] << 8) | (b[46] << 16) | ((uint32_t)b[47] << 24);
        if (verbose) {
            ESP_LOGI(TAG, "CCID: dwProtocols=0x%" PRIx32 " dwFeatures=0x%08" PRIx32 " dwMaxCCIDMessageLength=%" PRIu32 " bMaxCCIDBusySlots=%u",
                     protocols, features, maxmsg, b[53]);
        }
        if ((features & 0x00070000) != 0x00040000 || maxmsg > YD_CCID_MAX_MSG) {
            ESP_LOGW(TAG, YD_L("This key's CCID properties differ from what the receiver assumes (APDU level, max %d). If it does not work, please send this log", "この YubiKey の CCID の性質がレシーバーの想定（APDU単位・最大%d）と違います。動かなければログを送ってください"),
                     YD_CCID_MAX_MSG);
        }
    }

    esp_err_t err = usb_host_interface_claim(client, dev, itf->bInterfaceNumber, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, YD_L("Failed to claim the CCID interface: %s", "CCIDインターフェースの取得に失敗: %s"), esp_err_to_name(err));
        return false;
    }
    xSemaphoreTake(c.mutex, portMAX_DELAY);
    c.client = client;
    c.dev = dev;
    c.itf = itf->bInterfaceNumber;
    c.ep_in = ep_in;
    c.ep_out = ep_out;
    c.mps = mps;
    s_up_len = 0;
    s_up_discard = false;
    s_card_fresh = true;              /* 列挙し直したカードは、前の PIN の状態を持っていない */
    s_blocked = false;
    c.xout->device_handle = dev;
    c.xout->bEndpointAddress = ep_out;
    c.xout->callback = out_cb;
    c.attached = true;
    c.in_flight = 0;
    for (int i = 0; i < IN_XFERS; i++) {
        usb_transfer_t *x = c.xin[i];
        x->device_handle = dev;
        x->bEndpointAddress = ep_in;
        x->callback = in_cb;
        x->num_bytes = mps;
        c.in_flight++;
        if (usb_host_transfer_submit(x) != ESP_OK) {
            c.in_flight--;
        }
    }
    xSemaphoreGive(c.mutex);
    ESP_LOGI(TAG, YD_L("→ Claimed CCID (smart card): itf%u EP IN=0x%02x OUT=0x%02x MPS=%u", "→ CCID（スマートカード）を取得: itf%u EP IN=0x%02x OUT=0x%02x MPS=%u"), c.itf, ep_in, ep_out, mps);
    kick();
    return true;
}

void ccidh_detach(void)
{
    if (c.itf < 0) {
        return;
    }
    xSemaphoreTake(c.mutex, portMAX_DELAY);   /* 書き込み中なら終わるのを待つ */
    c.attached = false;
    usb_host_endpoint_halt(c.dev, c.ep_in);
    usb_host_endpoint_flush(c.dev, c.ep_in);
    for (int i = 0; i < 100 && c.in_flight > 0; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    usb_host_endpoint_clear(c.dev, c.ep_in);
    usb_host_interface_release(c.client, c.dev, c.itf);
    c.itf = -1;
    c.dev = NULL;
    s_up_len = 0;
    xSemaphoreGive(c.mutex);
    kick();
}

bool ccidh_present(void)
{
    return c.attached;
}

int64_t ccidh_last_activity_us(void)
{
    return s_last_us;
}

/* ======================================================================
 *  ccid タスク
 * ====================================================================== */
/* conn の接続に返す。その接続がもう切れていれば（送っている途中で切れても）新しい接続には送らない */
static void reply(uint32_t conn, const uint8_t *m, size_t n)
{
    esp_err_t err = yd_frag_send(conn, YD_CH_CCID, m, n);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, YD_L("Failed to send the response to the receiver: %s", "レシーバーへの応答送信に失敗: %s"), esp_err_to_name(err));
    }
}

static void reply_absent(uint32_t conn, const uint8_t *req)
{
    uint8_t r[YD_CCID_HDR];
    yd_ccid_make_absent_resp(req, r);
    reply(conn, r, sizeof(r));
}

static esp_err_t write_key(const uint8_t *m, size_t n)
{
    esp_err_t err = ESP_ERR_INVALID_STATE;
    xSemaphoreTake(c.mutex, portMAX_DELAY);
    if (c.attached) {
        xSemaphoreTake(c.out_done, 0);
        memcpy(c.xout->data_buffer, m, n);
        c.xout->num_bytes = n;
        err = usb_host_transfer_submit(c.xout);
        if (err == ESP_OK) {
            if (xSemaphoreTake(c.out_done, pdMS_TO_TICKS(OUT_TIMEOUT_MS)) != pdTRUE) {
                err = ESP_ERR_TIMEOUT;
            } else if (c.xout->status != USB_TRANSFER_STATUS_COMPLETED) {
                err = ESP_FAIL;
            }
        }
    }
    xSemaphoreGive(c.mutex);
    return err;
}

/* 自分で作った PowerOff / PowerOn を YubiKey に送り、その応答を待つ（ccid タスク） */
static bool card_power(uint8_t type)
{
    uint8_t req[YD_CCID_HDR];
    yd_ccid_make_power_req(type, s_own_seq++, req);
    esp_err_t err = write_key(req, sizeof(req));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, YD_L("Card reset: write of %s failed (%s)", "カードのリセット: %s の書き込みに失敗 (%s)"), type_name(type), esp_err_to_name(err));
        return false;
    }
    int64_t deadline = esp_timer_get_time() + (int64_t)POWER_TIMEOUT_MS * 1000;
    for (;;) {
        int64_t left_ms = (deadline - esp_timer_get_time()) / 1000;
        if (left_ms <= 0) {
            ESP_LOGW(TAG, YD_L("Card reset: no response to %s", "カードのリセット: %s に応答がない"), type_name(type));
            return false;
        }
        size_t n = xMessageBufferReceive(s_up_mb, s_rsp, sizeof(s_rsp), pdMS_TO_TICKS(left_ms) + 1);
        if (n == 0) {
            continue;
        }
        switch (yd_ccid_power_resp(req, s_rsp, n)) {
        case YD_CCID_POWER_OK:
            return true;
        case YD_CCID_POWER_FAIL:
            ESP_LOGW(TAG, YD_L("Card reset: %s refused (status=0x%02x err=0x%02x)", "カードのリセット: %s が拒否された (status=0x%02x err=0x%02x)"),
                     type_name(type), s_rsp[7], s_rsp[8]);
            return false;
        case YD_CCID_POWER_WAIT:
            break;                    /* 時間延長、前の接続の要求への遅れた応答（捨てる） */
        }
    }
}

/* conn の要求を通す前に、カードの状態を conn のものにする（ccid タスク）。前の接続が PIN を確かめていても、
 * 新しい接続はそれを使えない */
static void card_take(uint32_t conn)
{
    if (s_card_fresh) {
        s_card_fresh = false;
        s_card_conn = conn;
        return;
    }
    if (s_blocked || s_card_conn == conn) {
        return;
    }
    s_pend.active = false;            /* 前の接続の要求への応答は返さない */
    ESP_LOGI(TAG, YD_L("New connection from the receiver: resetting the card (PowerOff / PowerOn)", "レシーバーとの新しい接続: カードをリセットします（PowerOff / PowerOn）"));
    if (card_power(CCID_PC_POWER_OFF) && card_power(CCID_PC_POWER_ON)) {
        s_card_conn = conn;
        return;
    }
    s_blocked = true;
    ESP_LOGW(TAG, YD_L("Could not reset the card: answering \"no card\" until the key's USB bus has been restarted",
                       "カードをリセットできませんでした。YubiKey の USB バスを止めて再開するまで「カードなし」で応答します"));
    if (s_bus_reset) {
        s_bus_reset();
    }
}

static void handle_req(const uint8_t *m, size_t n, uint32_t conn)
{
    if (conn != yd_link_conn_id()) {
        ESP_LOGW(TAG, YD_L("Dropping a request from a closed connection", "切れた接続からの要求を破棄"));
        return;                       /* 返す相手がいない。新しい接続に流さない */
    }
    if (!yd_ccid_valid(m, n)) {
        ESP_LOGW(TAG, YD_L("Dropping malformed request (len=%u)", "形式不正の要求を破棄 (len=%u)"), (unsigned)n);
        return;
    }
    s_last_us = esp_timer_get_time();
    if (c.attached) {
        card_take(conn);
    }
    if (!c.attached || s_blocked) {
        ESP_LOGW(TAG, YD_L("%s received but CCID is unavailable: replying \"no card\"", "%s が届いたがCCIDが使えないので「カードなし」で応答"), type_name(m[0]));
        reply_absent(conn, m);
        return;
    }
    s_pend.active = true;
    s_pend.type = m[0];
    s_pend.seq = m[6];
    s_pend.conn = conn;
    s_pend.t0 = esp_timer_get_time();
    esp_err_t err = write_key(m, n);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, YD_L("Write to the key failed (%s). %s seq=%u", "YubiKeyへの書き込み失敗 (%s)。%s seq=%u"), esp_err_to_name(err), type_name(m[0]), m[6]);
        s_pend.active = false;
        reply_absent(conn, m);
        return;
    }
    ESP_LOGI(TAG, YD_L("Host→key %s seq=%u len=%u", "Mac→Key %s seq=%u len=%u"), type_name(m[0]), m[6], (unsigned)(n - YD_CCID_HDR));
}

static void handle_rsp(const uint8_t *m, size_t n)
{
    s_last_us = esp_timer_get_time();
    if (!s_pend.active || s_pend.conn != yd_link_conn_id()) {
        ESP_LOGW(TAG, YD_L("Dropping a response from the key with no request on this connection (seq=%u)", "この接続の要求がない YubiKey の応答を破棄 (seq=%u)"), m[6]);
        return;                       /* 切れた接続の要求への応答（タッチ待ちだった署名など）を新しい接続に返さない */
    }
    bool te = (m[7] & 0xC0) == CCID_CMD_TIME_EXT;
    if (te) {
        static int64_t last_te_log;
        if (s_last_us - last_te_log > 3000000) {
            last_te_log = s_last_us;
            ESP_LOGI(TAG, YD_L("Key→host time extension (waiting for touch etc.) seq=%u", "Key→Mac 時間延長（タッチ待ちなど）seq=%u"), m[6]);
        }
    } else {
        int64_t ms = (s_pend.seq == m[6]) ? (s_last_us - s_pend.t0) / 1000 : -1;
        ESP_LOGI(TAG, YD_L("Key→host 0x%02x seq=%u len=%u status=0x%02x err=0x%02x (%lldms)", "Key→Mac 0x%02x seq=%u len=%u status=0x%02x err=0x%02x（%lldms）"),
                 m[0], m[6], (unsigned)(n - YD_CCID_HDR), m[7], m[8], ms);
        s_pend.active = false;
    }
    reply(s_pend.conn, m, n);
}

static void ccid_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        size_t n;
        bool any;
        do {
            any = false;
            /* 応答を先に（時間延長のあとに本当の応答が続くことがある） */
            while ((n = xMessageBufferReceive(s_up_mb, s_rsp, sizeof(s_rsp), 0)) > 0) {
                handle_rsp(s_rsp, n);
                any = true;
            }
            if ((n = xMessageBufferReceive(s_down_mb, s_req, sizeof(s_req), 0)) > CONN_TAG) {
                uint32_t conn;
                memcpy(&conn, s_req, CONN_TAG);
                handle_req(s_req + CONN_TAG, n - CONN_TAG, conn);
                any = true;
            }
        } while (any);
    }
}

void ccidh_from_link(const uint8_t *data, uint16_t len)
{
    uint32_t conn = yd_link_conn_id();
    if (conn != s_down_conn) {
        s_down_conn = conn;
        yd_frag_rx_reset(&s_down_rx);  /* 前の接続で組み立て途中だった分は捨てる */
    }
    int n = yd_frag_rx_feed(&s_down_rx, data, len);
    if (n < 0) {
        ESP_LOGW(TAG, YD_L("Request from the receiver was incomplete: dropped", "レシーバーからの要求が欠けていたので破棄"));
        return;
    }
    if (n > 0) {
        memcpy(s_down_asm, &conn, CONN_TAG);
        if (xMessageBufferSend(s_down_mb, s_down_asm, CONN_TAG + n, pdMS_TO_TICKS(200)) != (size_t)(CONN_TAG + n)) {
            ESP_LOGW(TAG, YD_L("Request queue full: dropped", "要求キューが一杯のため破棄"));
        }
        kick();
    }
}

void ccidh_init(void (*bus_reset)(void))
{
    s_bus_reset = bus_reset;
    yd_frag_rx_init(&s_down_rx, s_down_asm + CONN_TAG, YD_CCID_BUF);
    s_down_conn = yd_link_conn_id();
    c.out_done = xSemaphoreCreateBinary();
    c.mutex = xSemaphoreCreateMutex();
    s_down_mb = xMessageBufferCreate(MB_SIZE);
    s_up_mb = xMessageBufferCreate(MB_SIZE);
    configASSERT(c.out_done && c.mutex && s_down_mb && s_up_mb);
    for (int i = 0; i < IN_XFERS; i++) {
        ESP_ERROR_CHECK(usb_host_transfer_alloc(64, 0, &c.xin[i]));
    }
    ESP_ERROR_CHECK(usb_host_transfer_alloc(YD_CCID_BUF, 0, &c.xout));
    xTaskCreatePinnedToCore(ccid_task, "ccid", 4096, NULL, 8, &s_task, 0);
}
