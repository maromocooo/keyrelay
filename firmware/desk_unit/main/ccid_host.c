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
#define MB_SIZE         (2 * (YD_CCID_BUF + 8))

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

/* レシーバー → 組み立て（yd_link 受信タスク） */
static uint8_t s_down_asm[YD_CCID_BUF];
static yd_frag_rx_t s_down_rx;

/* ccid タスク */
static uint8_t s_req[YD_CCID_BUF], s_rsp[YD_CCID_BUF];
static MessageBufferHandle_t s_down_mb, s_up_mb;
static TaskHandle_t s_task;
static volatile int64_t s_last_us;
static struct {
    bool active;
    uint8_t type, seq;
    int64_t t0;
} s_pend;

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
static void reply(const uint8_t *m, size_t n)
{
    esp_err_t err = yd_frag_send(YD_CH_CCID, m, n);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, YD_L("Failed to send the response to the receiver: %s", "レシーバーへの応答送信に失敗: %s"), esp_err_to_name(err));
    }
}

static void reply_absent(const uint8_t *req)
{
    uint8_t r[YD_CCID_HDR];
    yd_ccid_make_absent_resp(req, r);
    reply(r, sizeof(r));
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

static void handle_req(const uint8_t *m, size_t n)
{
    if (!yd_ccid_valid(m, n)) {
        ESP_LOGW(TAG, YD_L("Dropping malformed request (len=%u)", "形式不正の要求を破棄 (len=%u)"), (unsigned)n);
        return;
    }
    s_last_us = esp_timer_get_time();
    if (!c.attached) {
        ESP_LOGW(TAG, YD_L("%s received but CCID is unavailable: replying \"no card\"", "%s が届いたがCCIDが使えないので「カードなし」で応答"), type_name(m[0]));
        reply_absent(m);
        return;
    }
    s_pend.active = true;
    s_pend.type = m[0];
    s_pend.seq = m[6];
    s_pend.t0 = esp_timer_get_time();
    esp_err_t err = write_key(m, n);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, YD_L("Write to the key failed (%s). %s seq=%u", "YubiKeyへの書き込み失敗 (%s)。%s seq=%u"), esp_err_to_name(err), type_name(m[0]), m[6]);
        s_pend.active = false;
        reply_absent(m);
        return;
    }
    ESP_LOGI(TAG, "Mac→Key %s seq=%u len=%u", type_name(m[0]), m[6], (unsigned)(n - YD_CCID_HDR));
}

static void handle_rsp(const uint8_t *m, size_t n)
{
    s_last_us = esp_timer_get_time();
    bool te = (m[7] & 0xC0) == CCID_CMD_TIME_EXT;
    if (te) {
        static int64_t last_te_log;
        if (s_last_us - last_te_log > 3000000) {
            last_te_log = s_last_us;
            ESP_LOGI(TAG, YD_L("Key→host time extension (waiting for touch etc.) seq=%u", "Key→Mac 時間延長（タッチ待ちなど）seq=%u"), m[6]);
        }
    } else {
        int64_t ms = (s_pend.active && s_pend.seq == m[6]) ? (s_last_us - s_pend.t0) / 1000 : -1;
        ESP_LOGI(TAG, YD_L("Key→host 0x%02x seq=%u len=%u status=0x%02x err=0x%02x (%lldms)", "Key→Mac 0x%02x seq=%u len=%u status=0x%02x err=0x%02x（%lldms）"),
                 m[0], m[6], (unsigned)(n - YD_CCID_HDR), m[7], m[8], ms);
        s_pend.active = false;
    }
    reply(m, n);
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
            if ((n = xMessageBufferReceive(s_down_mb, s_req, sizeof(s_req), 0)) > 0) {
                handle_req(s_req, n);
                any = true;
            }
        } while (any);
    }
}

void ccidh_from_link(const uint8_t *data, uint16_t len)
{
    int n = yd_frag_rx_feed(&s_down_rx, data, len);
    if (n < 0) {
        ESP_LOGW(TAG, YD_L("Request from the receiver was incomplete: dropped", "レシーバーからの要求が欠けていたので破棄"));
        return;
    }
    if (n > 0) {
        if (xMessageBufferSend(s_down_mb, s_down_asm, n, pdMS_TO_TICKS(200)) != (size_t)n) {
            ESP_LOGW(TAG, YD_L("Request queue full: dropped", "要求キューが一杯のため破棄"));
        }
        kick();
    }
}

void ccidh_init(void)
{
    yd_frag_rx_init(&s_down_rx, s_down_asm, sizeof(s_down_asm));
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
