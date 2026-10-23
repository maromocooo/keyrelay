/*
 * ccid_dev: Mac ⇔ レシーバー の CCID（スマートカードリーダー）
 *
 *   Mac ──bulk OUT──▶ [組み立て] ──▶ ccid タスク ──yd_frag──▶ 机上ユニット ──▶ YubiKey
 *   Mac ◀──bulk IN─── ccid タスク ◀──[組み立て]◀──yd_frag── 机上ユニット ◀── YubiKey
 *   Mac ◀──interrupt── カードの有無が変わったとき NotifySlotChange
 *
 * - 要求は同時に1つ（bMaxCCIDBusySlots = 1）。処理中に次が来たら「スロット使用中」で返す
 * - 応答が遅いとき（BLE の再送・タッチ待ちなど）は自分で「時間延長」を返して Mac を待たせる
 * - 机上ユニットに届かないときは「カードなし」で自分で答える
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/message_buffer.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "tusb.h"
#include "device/usbd_pvt.h"
#include "yd_link.h"
#include "yd_frag.h"
#include "yd_ccid.h"
#include "ccid_dev.h"

static const char *TAG = "ccid";

#define LOCAL_TE_MS        1000     /* 机上ユニットから何も来ないままこれだけ経ったら時間延長を返す */
#define PENDING_TIMEOUT_MS 30000    /* これだけ何も来なければ失敗として返す */
#define IN_TIMEOUT_MS      5000     /* Mac が bulk IN を読み取るまでの上限 */
#define MB_SIZE            (2 * (YD_CCID_BUF + 8))

/* ---- エンドポイント（open で記録） ---- */
static uint8_t s_itf, s_ep_out, s_ep_in, s_ep_int;
static volatile bool s_opened;

/* ---- TinyUSB タスク側 ---- */
CFG_TUSB_MEM_SECTION CFG_TUSB_MEM_ALIGN static uint8_t s_out_pkt[CCID_EP_MPS];
static uint8_t s_out_msg[YD_CCID_BUF];
static size_t s_out_len;
static bool s_out_discard;

/* ---- ccid タスク側 ---- */
CFG_TUSB_MEM_SECTION CFG_TUSB_MEM_ALIGN static uint8_t s_in_buf[YD_CCID_BUF];
CFG_TUSB_MEM_SECTION CFG_TUSB_MEM_ALIGN static uint8_t s_int_buf[8];
static uint8_t s_req[YD_CCID_BUF];
static uint8_t s_rsp[YD_CCID_BUF];
static SemaphoreHandle_t s_in_sem;
static volatile xfer_result_t s_in_result;
static volatile bool s_int_busy;

/* ---- リンク受信タスク側 ---- */
static uint8_t s_link_asm[YD_CCID_BUF];
static yd_frag_rx_t s_link_rx;

static MessageBufferHandle_t s_host_mb;   /* Mac → 机上ユニット（TinyUSBタスク → ccidタスク） */
static MessageBufferHandle_t s_desk_mb;   /* 机上ユニット → Mac（リンク受信タスク → ccidタスク） */
static TaskHandle_t s_task;
static volatile bool s_card;               /* 机上ユニットが「CCIDが使える」と言っている */
static volatile bool s_reset_seen;

static inline int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static inline void kick(void)
{
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

/* ======================================================================
 *  TinyUSB クラスドライバー
 * ====================================================================== */
static void out_drop(const char *why)
{
    ESP_LOGW(TAG, "Macからのメッセージを破棄: %s", why);
    s_out_len = 0;
}

static void out_packet(const uint8_t *p, uint32_t n)
{
    if (s_out_discard) {
        if (n < CCID_EP_MPS) {
            s_out_discard = false;   /* 壊れたメッセージの最後まで読み捨てた */
        }
        return;
    }
    if (n == 0) {
        if (s_out_len) {
            out_drop("長さが足りない");
        }
        return;
    }
    if (s_out_len + n > sizeof(s_out_msg)) {
        out_drop("長すぎる");
        s_out_discard = (n == CCID_EP_MPS);
        return;
    }
    memcpy(s_out_msg + s_out_len, p, n);
    s_out_len += n;
    if (s_out_len >= YD_CCID_HDR) {
        uint32_t dl = yd_ccid_dwlen(s_out_msg);
        if (dl > YD_CCID_MAX_MSG - YD_CCID_HDR) {
            out_drop("dwLength が上限超え");
            s_out_discard = (n == CCID_EP_MPS);
            return;
        }
        size_t total = YD_CCID_HDR + dl;
        if (s_out_len >= total) {
            if (xMessageBufferSend(s_host_mb, s_out_msg, total, 0) != total) {
                ESP_LOGW(TAG, "要求キューが一杯のため破棄");
            }
            s_out_len = 0;
            kick();
            return;
        }
    }
    if (n < CCID_EP_MPS) {
        out_drop("途中で終わった");
    }
}

static void ccidd_init(void) {}

static bool ccidd_deinit(void)
{
    return true;
}

static void ccidd_reset(uint8_t rhport)
{
    (void)rhport;
    s_opened = false;
    s_out_len = 0;
    s_out_discard = false;
    s_int_busy = false;
    s_in_result = XFER_RESULT_FAILED;
    if (s_in_sem) {
        xSemaphoreGive(s_in_sem);   /* 送信待ちを起こす */
    }
    s_reset_seen = true;
    kick();
}

static uint16_t ccidd_open(uint8_t rhport, tusb_desc_interface_t const *itf, uint16_t max_len)
{
    TU_VERIFY(itf->bInterfaceClass == TUSB_CLASS_SMART_CARD, 0);
    uint8_t const *p = (uint8_t const *)itf;
    uint8_t const *end = p + max_len;
    uint16_t len = tu_desc_len(p);
    p = tu_desc_next(p);
    uint8_t found = 0;
    while (p < end && found < itf->bNumEndpoints && tu_desc_type(p) != TUSB_DESC_INTERFACE) {
        if (tu_desc_type(p) == TUSB_DESC_ENDPOINT) {
            tusb_desc_endpoint_t const *ep = (tusb_desc_endpoint_t const *)p;
            TU_ASSERT(usbd_edpt_open(rhport, ep), 0);
            if (ep->bmAttributes.xfer == TUSB_XFER_INTERRUPT) {
                s_ep_int = ep->bEndpointAddress;
            } else if (tu_edpt_dir(ep->bEndpointAddress) == TUSB_DIR_IN) {
                s_ep_in = ep->bEndpointAddress;
            } else {
                s_ep_out = ep->bEndpointAddress;
            }
            found++;
        }
        len += tu_desc_len(p);
        p = tu_desc_next(p);
    }
    TU_VERIFY(found == 3 && s_ep_in && s_ep_out && s_ep_int, 0);
    s_itf = itf->bInterfaceNumber;
    s_out_len = 0;
    s_out_discard = false;
    s_int_busy = false;
    TU_ASSERT(usbd_edpt_xfer(rhport, s_ep_out, s_out_pkt, CCID_EP_MPS, false), 0);
    s_opened = true;
    kick();
    return len;
}

static bool ccidd_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *req)
{
    if (req->bmRequestType_bit.type != TUSB_REQ_TYPE_CLASS) {
        return false;
    }
    if (stage != CONTROL_STAGE_SETUP) {
        return true;
    }
    switch (req->bRequest) {
    case 0x01:   /* ABORT: 続いて bulk で PC_to_RDR_Abort が来るので、ここは受け付けるだけ */
        return tud_control_status(rhport, req);
    default:     /* GET_CLOCK_FREQUENCIES / GET_DATA_RATES は非対応（ディスクリプタで0個と宣言） */
        return false;
    }
}

static bool ccidd_xfer_cb(uint8_t rhport, uint8_t ep_addr, xfer_result_t result, uint32_t n)
{
    if (ep_addr == s_ep_out) {
        if (result == XFER_RESULT_SUCCESS) {
            out_packet(s_out_pkt, n);
        }
        usbd_edpt_xfer(rhport, s_ep_out, s_out_pkt, CCID_EP_MPS, false);
    } else if (ep_addr == s_ep_in) {
        s_in_result = result;
        xSemaphoreGive(s_in_sem);
    } else if (ep_addr == s_ep_int) {
        s_int_busy = false;
        kick();
    }
    return true;
}

static const usbd_class_driver_t s_driver = {
    .init = ccidd_init,
    .deinit = ccidd_deinit,
    .reset = ccidd_reset,
    .open = ccidd_open,
    .control_xfer_cb = ccidd_control_xfer_cb,
    .xfer_cb = ccidd_xfer_cb,
};

const usbd_class_driver_t *ccid_dev_driver(void)
{
    return &s_driver;
}

/* ======================================================================
 *  Mac へ送る（ccid タスクだけが呼ぶ）
 * ====================================================================== */
static bool in_xfer(uint16_t len)
{
    xSemaphoreTake(s_in_sem, 0);
    if (!usbd_edpt_xfer(0, s_ep_in, s_in_buf, len, false)) {
        usbd_edpt_release(0, s_ep_in);
        return false;
    }
    if (xSemaphoreTake(s_in_sem, pdMS_TO_TICKS(IN_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "Macが応答を読み取りません");
        return false;
    }
    return s_in_result == XFER_RESULT_SUCCESS;
}

static bool send_to_mac(const uint8_t *msg, size_t n)
{
    if (!s_opened || n > sizeof(s_in_buf)) {
        return false;
    }
    /* 前の送信が終わっていない（Mac が読まない）ときは少し待つ。バッファはそのあとで書き換える */
    bool claimed = false;
    for (int i = 0; i < IN_TIMEOUT_MS / 20 && s_opened; i++) {
        if (usbd_edpt_claim(0, s_ep_in)) {
            claimed = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (!claimed) {
        ESP_LOGW(TAG, "bulk IN が空かないため応答を破棄 (type=0x%02x)", msg[0]);
        return false;
    }
    memcpy(s_in_buf, msg, n);
    if (!in_xfer((uint16_t)n)) {
        return false;
    }
    /* 64の倍数ぴったりのときは、メッセージの終わりを示す長さ0のパケットを続ける */
    if (n % CCID_EP_MPS == 0) {
        if (!usbd_edpt_claim(0, s_ep_in) || !in_xfer(0)) {
            return false;
        }
    }
    return true;
}

static void reply_local(const uint8_t *req, uint8_t status, uint8_t error)
{
    uint8_t r[YD_CCID_HDR];
    yd_ccid_make_resp(req, status, error, r);
    send_to_mac(r, sizeof(r));
}

static bool notify_slot(bool present)
{
    if (!s_opened || s_int_busy) {
        return false;
    }
    if (!usbd_edpt_claim(0, s_ep_int)) {
        return false;
    }
    s_int_buf[0] = CCID_RDR_NOTIFY_SLOT;
    s_int_buf[1] = (present ? 0x01 : 0x00) | 0x02;   /* bit0: カードあり, bit1: 変化した */
    s_int_busy = true;
    if (!usbd_edpt_xfer(0, s_ep_int, s_int_buf, 2, false)) {
        s_int_busy = false;
        usbd_edpt_release(0, s_ep_int);
        return false;
    }
    return true;
}

/* ======================================================================
 *  ccid タスク
 * ====================================================================== */
static struct {
    bool active;
    uint8_t type, seq;
    uint8_t hdr[YD_CCID_HDR];   /* 失敗応答・時間延長を作るための要求ヘッダー */
    int64_t t_start, t_desk, t_mac;
    uint32_t len;
} s_pend;

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

static void fail_pending(const char *why)
{
    if (!s_pend.active) {
        return;
    }
    ESP_LOGW(TAG, "%s seq=%u を失敗で返します（%s）", type_name(s_pend.type), s_pend.seq, why);
    reply_local(s_pend.hdr, CCID_CMD_FAILED | (s_card ? CCID_ICC_ACTIVE : CCID_ICC_ABSENT), CCID_ERR_ICC_MUTE);
    s_pend.active = false;
}

static void handle_host(const uint8_t *m, size_t n, bool card)
{
    if (!yd_ccid_valid(m, n)) {
        ESP_LOGW(TAG, "形式不正のメッセージを破棄 (len=%u)", (unsigned)n);
        return;
    }
    uint8_t type = m[0], seq = m[6];
    if (m[5] != 0) {
        reply_local(m, CCID_CMD_FAILED | CCID_ICC_ABSENT, CCID_ERR_BAD_SLOT);
        return;
    }
    if (!card) {
        uint8_t r[YD_CCID_HDR];
        yd_ccid_make_absent_resp(m, r);
        send_to_mac(r, sizeof(r));
        ESP_LOGD(TAG, "%s: YubiKeyなしとして応答", type_name(type));
        return;
    }
    if (s_pend.active && type != CCID_PC_ABORT) {
        ESP_LOGW(TAG, "処理中に %s が届いたので「使用中」で返します", type_name(type));
        reply_local(m, CCID_CMD_FAILED | CCID_ICC_ACTIVE, CCID_ERR_SLOT_BUSY);
        return;
    }
    esp_err_t err = yd_frag_send(YD_CH_CCID, m, n);
    if (err != ESP_OK) {
        uint8_t r[YD_CCID_HDR];
        yd_ccid_make_absent_resp(m, r);
        send_to_mac(r, sizeof(r));
        ESP_LOGW(TAG, "%s を机上ユニットへ送れませんでした (%s)", type_name(type), esp_err_to_name(err));
        return;
    }
    int64_t t = now_ms();
    s_pend.active = true;
    s_pend.type = type;
    s_pend.seq = seq;
    s_pend.len = n - YD_CCID_HDR;
    memcpy(s_pend.hdr, m, YD_CCID_HDR);
    s_pend.t_start = s_pend.t_desk = s_pend.t_mac = t;
    ESP_LOGI(TAG, "Mac→ %s seq=%u len=%u", type_name(type), seq, (unsigned)(n - YD_CCID_HDR));
}

static void handle_desk(const uint8_t *m, size_t n)
{
    if (n < YD_CCID_HDR || yd_ccid_dwlen(m) != n - YD_CCID_HDR) {
        ESP_LOGW(TAG, "机上ユニットから形式不正の応答 (len=%u)", (unsigned)n);
        return;
    }
    if (!s_pend.active || m[6] != s_pend.seq) {
        ESP_LOGW(TAG, "待っていない応答を破棄 (type=0x%02x seq=%u)", m[0], m[6]);
        return;
    }
    bool te = (m[7] & 0xC0) == CCID_CMD_TIME_EXT;
    s_pend.t_desk = now_ms();
    if (!send_to_mac(m, n)) {
        ESP_LOGW(TAG, "Macへの応答送信に失敗");
    }
    s_pend.t_mac = now_ms();
    if (te) {
        return;   /* YubiKey 自身の時間延長（タッチ待ちなど）。本当の応答を待ち続ける */
    }
    ESP_LOGI(TAG, "→Mac 0x%02x seq=%u len=%u status=0x%02x err=0x%02x（%lldms）", m[0], m[6],
             (unsigned)(n - YD_CCID_HDR), m[7], m[8], s_pend.t_mac - s_pend.t_start);
    s_pend.active = false;
}

static void ccid_task(void *arg)
{
    bool card_seen = false, opened_seen = false, notify_pending = false;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));

        if (s_reset_seen) {
            s_reset_seen = false;
            s_pend.active = false;
            xMessageBufferReset(s_host_mb);
        }
        bool card = s_card;
        bool opened = s_opened;
        if (card != card_seen) {
            card_seen = card;
            notify_pending = true;
            ESP_LOGI(TAG, "スマートカード: %s", card ? "あり（YubiKeyのCCIDが使えます）" : "なし");
            if (!card) {
                fail_pending("YubiKey/リンクが切れた");
                xMessageBufferReset(s_desk_mb);
            }
        }
        if (opened != opened_seen) {
            opened_seen = opened;
            notify_pending = opened;
        }
        if (notify_pending && opened && !s_int_busy) {
            if (notify_slot(card)) {
                notify_pending = false;   /* 送れなければ次の周期でもう一度 */
            }
        }

        size_t n;
        while ((n = xMessageBufferReceive(s_desk_mb, s_rsp, sizeof(s_rsp), 0)) > 0) {
            handle_desk(s_rsp, n);
        }
        while ((n = xMessageBufferReceive(s_host_mb, s_req, sizeof(s_req), 0)) > 0) {
            handle_host(s_req, n, card && opened);
            /* 処理中の間に届いた応答を先に流す（Abort などで続けて来たとき用） */
            while ((n = xMessageBufferReceive(s_desk_mb, s_rsp, sizeof(s_rsp), 0)) > 0) {
                handle_desk(s_rsp, n);
            }
        }

        if (s_pend.active) {
            int64_t t = now_ms();
            if (t - s_pend.t_desk > PENDING_TIMEOUT_MS) {
                fail_pending("机上ユニットから応答なし");
            } else if (t - s_pend.t_mac > LOCAL_TE_MS && yd_ccid_resp_type(s_pend.type) == CCID_RDR_DATA_BLOCK) {
                reply_local(s_pend.hdr, CCID_CMD_TIME_EXT | CCID_ICC_ACTIVE, 0x01);
                s_pend.t_mac = now_ms();
                ESP_LOGI(TAG, "応答待ち %lldms: Macに時間延長を送信", t - s_pend.t_start);
            }
        }
    }
}

/* ======================================================================
 *  外部 API
 * ====================================================================== */
void ccid_dev_set_card(bool available)
{
    if (s_card != available) {
        s_card = available;
        if (!available) {
            yd_frag_rx_reset(&s_link_rx);   /* 呼び出し元はリンク受信タスク（from_link と同じ） */
        }
        kick();
    }
}

void ccid_dev_from_link(const uint8_t *data, uint16_t len)
{
    int n = yd_frag_rx_feed(&s_link_rx, data, len);
    if (n < 0) {
        ESP_LOGW(TAG, "机上ユニットからの応答が欠けていたので破棄");
        return;
    }
    if (n > 0) {
        if (xMessageBufferSend(s_desk_mb, s_link_asm, n, pdMS_TO_TICKS(200)) != (size_t)n) {
            ESP_LOGW(TAG, "応答キューが一杯のため破棄");
        }
        kick();
    }
}

void ccid_dev_init(void)
{
    yd_frag_rx_init(&s_link_rx, s_link_asm, sizeof(s_link_asm));
    s_in_sem = xSemaphoreCreateBinary();
    s_host_mb = xMessageBufferCreate(MB_SIZE);
    s_desk_mb = xMessageBufferCreate(MB_SIZE);
    configASSERT(s_in_sem && s_host_mb && s_desk_mb);
    xTaskCreate(ccid_task, "ccid", 4096, NULL, 10, &s_task);
}
