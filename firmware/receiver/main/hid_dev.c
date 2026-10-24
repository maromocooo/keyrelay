/*
 * hid_dev: FIDO と OTP キーボードの HID インターフェース（自前ドライバー）
 *
 * ---- FIDO ----
 * interrupt OUT の64バイトを受けたら hid_dev_set_fido_rx のコールバックへ、IN は hid_dev_fido_send で送る。
 *
 * ---- キー入力 ----
 * 机上ユニットから「押す〜離す」のまとまりで届くレポートを、間を空けずに Mac へ流す。
 * 押されたまま KBD_STUCK_MS 続きが来なければ自分で「離す」を送る（キーリピート防止）。
 * 新しいタッチの最初に「英数」キーを送って、日本語入力のままでも化けないようにする（YD_OTP_FORCE_EISU）。
 * 机上ユニットは長さをそろえるために「全部離す」で埋めて送ってくるので、続く「全部離す」は1つにまとめる。
 *
 * 既定（CONFIG_YD_OTP_KBD_FILTER_MODHEX）では、Yubico OTP の形のもの（修飾キーなしの ModHex 32〜64 文字と、
 * そのあとの Enter 1回）しか打たない（kbd_filter.h）。PSK を盗んで机上ユニットになりすました相手が、
 * 任意のキー入力（コマンドや、ダイアログを閉じる Enter など）を打ち込むのを防ぐため（SECURITY.md）。
 * このときは、通したキーを1つずつ「押す」「全部離す」の2つのレポートにして打ち、机上ユニットの「離す」は
 * 使わない（キーを押しっぱなしにさせて、パソコンのキーリピートで文字を増やされないように）。
 *
 * ---- フィーチャーレポート ----
 *   Mac ──SET_REPORT(Feature)──▶ すぐ受け付けて、机上ユニットへ [SET, data]
 *   Mac ──GET_REPORT(Feature)──▶ 返事を保留して、机上ユニットへ [GET, tag]
 *                                ◀── [GET_RESP, tag, ok, data] が来たら、保留していた返事を返す
 * SET と GET は同じキューで順番に送るので、YubiKey には Mac が出した順に届く。
 * YubiKey の OTP の手順（書き込み可フラグを見て7バイトずつ書く→応答を読む）は ykman / ykpers の
 * ほうが「回数」で待つので、BLE の往復で1回あたり数十ms かかっても動く。
 */
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "yd_link.h"
#include "hid_dev.h"
#include "kbd_filter.h"
#include "yd_lang.h"

static const char *TAG = "otp";

#define KBD_REPORT_LEN      8
#define KBD_STUCK_MS        150
#define YD_OTP_FORCE_EISU   1
#define KBD_BURST_GAP_MS    1000
#define KBD_EISU_SETTLE_MS  30
#define GET_TIMEOUT_MS      1500    /* 机上ユニットからこれだけ返事がなければ STALL で返す */

static const uint8_t s_report_desc[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01,                     /* Generic Desktop / Keyboard */
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,  /* 修飾キー */
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01,                     /* 予約 */
    0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02,  /* LED */
    0x95, 0x01, 0x75, 0x03, 0x91, 0x01,
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x05, 0x07, 0x19, 0x00, 0x2A, 0xFF, 0x00, 0x81, 0x00,  /* キー×6 */
    0x09, 0x03, 0x75, 0x08, 0x95, 0x08, 0xB1, 0x02,         /* フィーチャー 8バイト（YubiKey の OTP 用） */
    0xC0,
};
_Static_assert(sizeof(s_report_desc) == OTPKBD_REPORT_DESC_LEN, "OTPKBD_REPORT_DESC_LEN");

static const uint8_t s_kbd_hid_desc[9] = {
    9, HID_DESC_TYPE_HID, U16_TO_U8S_LE(0x0111), 0, 1, HID_DESC_TYPE_REPORT, U16_TO_U8S_LE(OTPKBD_REPORT_DESC_LEN),
};

static const uint8_t s_fido_report_desc[] = {
    0x06, 0xD0, 0xF1, 0x09, 0x01, 0xA1, 0x01,               /* FIDO Alliance / U2F Authenticator */
    0x09, 0x20, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x40, 0x81, 0x02,   /* Input 64 */
    0x09, 0x21, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x40, 0x91, 0x02,   /* Output 64 */
    0xC0,
};
_Static_assert(sizeof(s_fido_report_desc) == FIDO_REPORT_DESC_LEN, "FIDO_REPORT_DESC_LEN");

static const uint8_t s_fido_hid_desc[9] = {
    9, HID_DESC_TYPE_HID, U16_TO_U8S_LE(0x0111), 0, 1, HID_DESC_TYPE_REPORT, U16_TO_U8S_LE(FIDO_REPORT_DESC_LEN),
};

/* ---- USB（キーボード） ---- */
static uint8_t s_itf = 0xFF, s_ep_in;
static volatile bool s_opened;

/* ---- USB（FIDO） ---- */
static uint8_t s_fido_itf = 0xFF, s_fido_ep_in, s_fido_ep_out;
static volatile bool s_fido_opened;
CFG_TUSB_MEM_SECTION CFG_TUSB_MEM_ALIGN static uint8_t s_fido_out_buf[FIDO_REPORT_LEN];
CFG_TUSB_MEM_SECTION CFG_TUSB_MEM_ALIGN static uint8_t s_fido_in_buf[FIDO_REPORT_LEN];
static SemaphoreHandle_t s_fido_sem;
static void (*s_fido_rx_cb)(const uint8_t *report);
static uint8_t s_protocol = 1, s_idle;
CFG_TUSB_MEM_SECTION CFG_TUSB_MEM_ALIGN static uint8_t s_in_buf[KBD_REPORT_LEN];
CFG_TUSB_MEM_SECTION CFG_TUSB_MEM_ALIGN static uint8_t s_ctrl_buf[KBD_REPORT_LEN];
CFG_TUSB_MEM_SECTION CFG_TUSB_MEM_ALIGN static uint8_t s_get_buf[KBD_REPORT_LEN];
static uint8_t s_small[1];
static SemaphoreHandle_t s_in_sem;
static volatile xfer_result_t s_in_result;   /* キーボードの IN 転送の結果（s_in_sem を渡す前に書く） */

/* ---- 保留中の GET_REPORT(Feature)（TinyUSB タスクで書き換え） ---- */
static volatile bool s_get_pending;
static volatile uint8_t s_get_tag;
static volatile int64_t s_get_t0;
static tusb_control_request_t s_get_req;     /* 保留した GET_REPORT の要求（返事の長さを wLength に合わせる） */

/* ---- キュー ---- */
typedef struct {
    uint8_t d[KBD_REPORT_LEN];
    uint32_t epoch;                 /* 入れたときの s_kbd_epoch（ModHex フィルターのときだけ使う） */
} kbd_rep_t;

typedef struct {
    uint8_t op, tag;
    uint8_t d[YD_OTPCTL_REPORT_LEN];
} ctl_req_t;

static QueueHandle_t s_kbd_q, s_ctl_q;
static TaskHandle_t s_ctl_task;
static volatile bool s_kbd_on, s_ctl_on;
static uint32_t s_ctl_count;
#if CONFIG_YD_OTP_KBD_FILTER_MODHEX
static uint32_t s_kbd_blocked;      /* kbd_filter.h で止めたレポートの数 */
/* kbd_task が Mac に送れなかったレポートの数（kbd_task だけが書く）。フィルターは送れなかった文字も数えているので、
 * これが変わったら rx_task はそのまとまりを打たず、kbd_task は古い値で入れられたレポートを送らない */
static volatile uint32_t s_kbd_epoch;
#else
static bool s_kbd_q_zero = true;    /* 最後にキューに入れたレポートが全部 0（rx_task だけが触る） */
#endif

static inline int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

/* ======================================================================
 *  保留していた GET_REPORT に答える（TinyUSB タスクで実行される）
 * ====================================================================== */
static void get_finish(void *param)
{
    uint32_t v = (uint32_t)(uintptr_t)param;
    uint8_t tag = v & 0xFF;
    bool ok = (v >> 8) & 1;
    if (!s_get_pending || tag != s_get_tag) {
        return;   /* もう待っていない（タイムアウト済み・新しい要求） */
    }
    s_get_pending = false;
    if (ok) {
        /* 受けたときの要求を渡す。TinyUSB の版によっては、渡した要求の wLength で返す長さを決める */
        tud_control_xfer(0, &s_get_req, s_get_buf, KBD_REPORT_LEN);
    } else {
        usbd_edpt_stall(0, 0x80);
        usbd_edpt_stall(0, 0x00);
    }
}

static void ctl_push(uint8_t op, uint8_t tag, const uint8_t *d)
{
    ctl_req_t r = { .op = op, .tag = tag };
    if (d) {
        memcpy(r.d, d, YD_OTPCTL_REPORT_LEN);
    }
    if (xQueueSend(s_ctl_q, &r, 0) != pdTRUE) {
        ESP_LOGW(TAG, YD_L("Feature report queue full", "フィーチャーレポートのキューが一杯"));
    }
    if (s_ctl_task) {
        xTaskNotifyGive(s_ctl_task);
    }
}

/* ======================================================================
 *  クラスドライバー
 * ====================================================================== */
static void otpd_init(void) {}

static bool otpd_deinit(void)
{
    return true;
}

static void otpd_reset(uint8_t rhport)
{
    (void)rhport;
    s_opened = false;
    s_fido_opened = false;
    s_itf = s_fido_itf = 0xFF;
    if (s_fido_sem) {
        xSemaphoreGive(s_fido_sem);
    }
    s_get_pending = false;
    s_protocol = 1;
    s_in_result = XFER_RESULT_FAILED;
    if (s_in_sem) {
        xSemaphoreGive(s_in_sem);
    }
}

static uint16_t otpd_open(uint8_t rhport, tusb_desc_interface_t const *itf, uint16_t max_len)
{
    TU_VERIFY(itf->bInterfaceClass == TUSB_CLASS_HID, 0);
    bool kbd = itf->bInterfaceSubClass == 1 && itf->bInterfaceProtocol == 1 && itf->bNumEndpoints == 1;
    bool fido = itf->bInterfaceProtocol == 0 && itf->bNumEndpoints == 2;
    TU_VERIFY(kbd || fido, 0);
    uint8_t const *p = (uint8_t const *)itf;
    uint8_t const *end = p + max_len;
    uint16_t len = tu_desc_len(p);
    p = tu_desc_next(p);
    uint8_t found = 0;
    while (p < end && found < itf->bNumEndpoints && tu_desc_type(p) != TUSB_DESC_INTERFACE) {
        if (tu_desc_type(p) == TUSB_DESC_ENDPOINT) {
            tusb_desc_endpoint_t const *ep = (tusb_desc_endpoint_t const *)p;
            TU_ASSERT(usbd_edpt_open(rhport, ep), 0);
            bool in = tu_edpt_dir(ep->bEndpointAddress) == TUSB_DIR_IN;
            if (kbd && in) {
                s_ep_in = ep->bEndpointAddress;
            } else if (fido && in) {
                s_fido_ep_in = ep->bEndpointAddress;
            } else if (fido) {
                s_fido_ep_out = ep->bEndpointAddress;
            }
            found++;
        }
        len += tu_desc_len(p);   /* HID ディスクリプタも含める */
        p = tu_desc_next(p);
    }
    TU_VERIFY(found == itf->bNumEndpoints, 0);
    if (kbd) {
        s_itf = itf->bInterfaceNumber;
        s_protocol = 1;
        s_opened = true;
    } else {
        s_fido_itf = itf->bInterfaceNumber;
        TU_ASSERT(usbd_edpt_xfer(rhport, s_fido_ep_out, s_fido_out_buf, FIDO_REPORT_LEN, false), 0);
        s_fido_opened = true;
    }
    return len;
}

static bool otpd_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *req)
{
    uint8_t itf = tu_u16_low(req->wIndex);
    if (req->bmRequestType_bit.recipient != TUSB_REQ_RCPT_INTERFACE || (itf != s_itf && itf != s_fido_itf)) {
        return false;
    }
    bool is_fido = (itf == s_fido_itf);
    if (req->bmRequestType_bit.type == TUSB_REQ_TYPE_STANDARD) {
        if (req->bRequest != TUSB_REQ_GET_DESCRIPTOR) {
            return false;
        }
        if (stage == CONTROL_STAGE_SETUP) {
            uint8_t type = tu_u16_high(req->wValue);
            if (type == HID_DESC_TYPE_REPORT) {
                return is_fido ? tud_control_xfer(rhport, req, (void *)s_fido_report_desc, sizeof(s_fido_report_desc))
                               : tud_control_xfer(rhport, req, (void *)s_report_desc, sizeof(s_report_desc));
            }
            if (type == HID_DESC_TYPE_HID) {
                return tud_control_xfer(rhport, req, (void *)(is_fido ? s_fido_hid_desc : s_kbd_hid_desc), 9);
            }
            return false;
        }
        return true;
    }
    if (req->bmRequestType_bit.type != TUSB_REQ_TYPE_CLASS) {
        return false;
    }
    uint8_t rtype = tu_u16_high(req->wValue);
    switch (req->bRequest) {
    case HID_REQ_CONTROL_GET_REPORT:
        if (stage != CONTROL_STAGE_SETUP) {
            return true;
        }
        if (rtype == HID_REPORT_TYPE_FEATURE) {
            if (!s_ctl_on) {
                return false;   /* 机上ユニットが未対応・未接続 → STALL */
            }
            s_get_req = *req;
            s_get_tag++;
            s_get_pending = true;
            s_get_t0 = now_ms();
            ctl_push(YD_OTPCTL_GET, s_get_tag, NULL);
            return true;        /* 返事は get_finish() であとから */
        }
        if (rtype == HID_REPORT_TYPE_INPUT && !is_fido) {
            memset(s_ctrl_buf, 0, sizeof(s_ctrl_buf));
            return tud_control_xfer(rhport, req, s_ctrl_buf, KBD_REPORT_LEN);
        }
        return false;
    case HID_REQ_CONTROL_SET_REPORT:
        if (stage == CONTROL_STAGE_SETUP) {
            TU_VERIFY(req->wLength <= sizeof(s_ctrl_buf));   /* FIDO の出力レポートを制御転送で送る使い方は非対応 */
            return tud_control_xfer(rhport, req, s_ctrl_buf, req->wLength);
        }
        if (stage == CONTROL_STAGE_DATA && rtype == HID_REPORT_TYPE_FEATURE) {
            if (!s_ctl_on || req->wLength < YD_OTPCTL_REPORT_LEN) {
                return false;
            }
            ctl_push(YD_OTPCTL_SET, 0, s_ctrl_buf);
        }
        return true;   /* 出力レポート（キーボードの LED）は使わない */
    case HID_REQ_CONTROL_SET_IDLE:
        if (stage == CONTROL_STAGE_SETUP) {
            s_idle = tu_u16_high(req->wValue);
            return tud_control_status(rhport, req);
        }
        return true;
    case HID_REQ_CONTROL_GET_IDLE:
        if (stage == CONTROL_STAGE_SETUP) {
            s_small[0] = s_idle;
            return tud_control_xfer(rhport, req, s_small, 1);
        }
        return true;
    case HID_REQ_CONTROL_SET_PROTOCOL:
        if (stage == CONTROL_STAGE_SETUP) {
            s_protocol = (uint8_t)tu_u16_low(req->wValue);
            return tud_control_status(rhport, req);
        }
        return true;
    case HID_REQ_CONTROL_GET_PROTOCOL:
        if (stage == CONTROL_STAGE_SETUP) {
            s_small[0] = s_protocol;
            return tud_control_xfer(rhport, req, s_small, 1);
        }
        return true;
    default:
        return false;
    }
}

static bool otpd_xfer_cb(uint8_t rhport, uint8_t ep_addr, xfer_result_t result, uint32_t n)
{
    if (ep_addr == s_fido_ep_out && s_fido_opened) {
        if (result == XFER_RESULT_SUCCESS && n > 0 && s_fido_rx_cb) {
            uint8_t rep[FIDO_REPORT_LEN] = { 0 };
            memcpy(rep, s_fido_out_buf, n > FIDO_REPORT_LEN ? FIDO_REPORT_LEN : n);
            s_fido_rx_cb(rep);
        }
        usbd_edpt_xfer(rhport, s_fido_ep_out, s_fido_out_buf, FIDO_REPORT_LEN, false);
    } else if (ep_addr == s_fido_ep_in) {
        xSemaphoreGive(s_fido_sem);
    } else if (ep_addr == s_ep_in) {
        s_in_result = result;
        xSemaphoreGive(s_in_sem);
    }
    return true;
}

static const usbd_class_driver_t s_driver = {
    .init = otpd_init,
    .deinit = otpd_deinit,
    .reset = otpd_reset,
    .open = otpd_open,
    .control_xfer_cb = otpd_control_xfer_cb,
    .xfer_cb = otpd_xfer_cb,
};

const usbd_class_driver_t *hid_dev_driver(void)
{
    return &s_driver;
}

/* ======================================================================
 *  FIDO
 * ====================================================================== */
void hid_dev_set_fido_rx(void (*cb)(const uint8_t *report))
{
    s_fido_rx_cb = cb;
}

bool hid_dev_mounted(void)
{
    return tud_mounted() && s_fido_opened;
}

bool hid_dev_fido_send(const uint8_t *report, uint32_t timeout_ms)
{
    TickType_t start = xTaskGetTickCount();
    while ((xTaskGetTickCount() - start) < pdMS_TO_TICKS(timeout_ms)) {
        if (s_fido_opened && tud_mounted() && usbd_edpt_claim(0, s_fido_ep_in)) {
            memcpy(s_fido_in_buf, report, FIDO_REPORT_LEN);
            xSemaphoreTake(s_fido_sem, 0);
            if (usbd_edpt_xfer(0, s_fido_ep_in, s_fido_in_buf, FIDO_REPORT_LEN, false)) {
                /* Mac が受け取るまで待ってから次へ（順序と取りこぼし防止） */
                if (xSemaphoreTake(s_fido_sem, pdMS_TO_TICKS(500)) != pdTRUE) {
                    ESP_LOGW(TAG, YD_L("FIDO: no completion for the IN report", "FIDO: INレポートの完了通知なし"));
                }
                return true;
            }
            usbd_edpt_release(0, s_fido_ep_in);
        }
        vTaskDelay(1);
    }
    return false;
}

/* ======================================================================
 *  キー入力 → Mac
 * ====================================================================== */
/* Mac が受け取ったら true。送り始められない、完了しない、完了したが失敗した、のどれも false（ModHex フィルターは
 * 送った文字を数えているので、届いたか分からないものを届いたことにしない）。完了を待ちきれなかった転送が
 * あとで終わっても、その文字は届かなかったものとして扱う（そのまとまりを捨てるほうに倒れる） */
static bool kbd_send(const uint8_t *d)
{
    TickType_t start = xTaskGetTickCount();
    while ((xTaskGetTickCount() - start) < pdMS_TO_TICKS(500)) {
        if (s_opened && tud_mounted() && usbd_edpt_claim(0, s_ep_in)) {
            memcpy(s_in_buf, d, KBD_REPORT_LEN);
            xSemaphoreTake(s_in_sem, 0);
            if (usbd_edpt_xfer(0, s_ep_in, s_in_buf, KBD_REPORT_LEN, false)) {
                return xSemaphoreTake(s_in_sem, pdMS_TO_TICKS(100)) == pdTRUE && s_in_result == XFER_RESULT_SUCCESS;
            }
            usbd_edpt_release(0, s_ep_in);
        }
        vTaskDelay(1);
    }
    return false;
}

static bool kbd_has_keys(const uint8_t *d)
{
    for (int i = 2; i < KBD_REPORT_LEN; i++) {
        if (d[i]) {
            return true;
        }
    }
    return false;
}

static bool kbd_all_zero(const uint8_t *d)
{
    for (int i = 0; i < KBD_REPORT_LEN; i++) {
        if (d[i]) {
            return false;
        }
    }
    return true;
}

static void kbd_task(void *arg)
{
    kbd_rep_t r;
    bool pressed = false, first = true, sent_zero = true;
    TickType_t last = 0;
    for (;;) {
        if (xQueueReceive(s_kbd_q, &r, pressed ? pdMS_TO_TICKS(KBD_STUCK_MS) : portMAX_DELAY) != pdTRUE) {
            static const uint8_t zero[KBD_REPORT_LEN] = { 0 };
            ESP_LOGW(TAG, YD_L("Key held without further reports: releasing", "キーが押されたまま続きが来ないので離します"));
            kbd_send(zero);
            pressed = false;
            sent_zero = true;
            continue;
        }
        bool zero = kbd_all_zero(r.d);
        if (zero && sent_zero) {
            continue;                   /* 埋め草（続けて来る「全部離す」） */
        }
#if CONFIG_YD_OTP_KBD_FILTER_MODHEX
        if (!zero && r.epoch != s_kbd_epoch) {
            continue;                   /* 送れなかった文字より前に入っていた同じまとまりの残り（Enter を含む） */
        }
#endif
#if YD_OTP_FORCE_EISU
        if (kbd_has_keys(r.d) && (first || (xTaskGetTickCount() - last) > pdMS_TO_TICKS(KBD_BURST_GAP_MS))) {
            static const uint8_t eisu[KBD_REPORT_LEN] = { 0, 0, HID_KEY_LANG2 };
            static const uint8_t up[KBD_REPORT_LEN] = { 0 };
            kbd_send(eisu);
            kbd_send(up);
            vTaskDelay(pdMS_TO_TICKS(KBD_EISU_SETTLE_MS));
            ESP_LOGI(TAG, YD_L("OTP: sent the Eisu (LANG2) key first", "OTP入力: 先に「英数」キーを送りました"));
        }
#endif
        if (!kbd_send(r.d)) {
            ESP_LOGW(TAG, YD_L("Keyboard report not delivered (not connected to the host?)", "キーボードのレポートを Mac に届けられませんでした（Macに未接続？）"));
#if CONFIG_YD_OTP_KBD_FILTER_MODHEX
            s_kbd_epoch++;
            continue;                   /* Mac が見ている状態は変わっていない（押したままなら KBD_STUCK_MS で離す） */
#endif
        }
        sent_zero = zero;
        pressed = kbd_has_keys(r.d);
        last = xTaskGetTickCount();
        first = false;
    }
}

/* ======================================================================
 *  フィーチャーレポート → 机上ユニット
 * ====================================================================== */
static void ctl_task(void *arg)
{
    ctl_req_t r;
    int64_t last_log = 0;
    for (;;) {
        if (xQueueReceive(s_ctl_q, &r, pdMS_TO_TICKS(100)) == pdTRUE) {
            uint8_t m[1 + YD_OTPCTL_REPORT_LEN];
            uint16_t n;
            m[0] = r.op;
            if (r.op == YD_OTPCTL_SET) {
                memcpy(&m[1], r.d, YD_OTPCTL_REPORT_LEN);
                n = 1 + YD_OTPCTL_REPORT_LEN;
            } else {
                m[1] = r.tag;
                n = 2;
            }
            esp_err_t err = yd_link_send(YD_CH_OTPCTL, m, n);
            if (err != ESP_OK && r.op == YD_OTPCTL_GET) {
                usbd_defer_func(get_finish, (void *)(uintptr_t)r.tag, false);   /* ok=0 → STALL */
            }
            s_ctl_count++;
            int64_t t = now_ms();
            if (t - last_log > 3000) {
                last_log = t;
                ESP_LOGI(TAG, YD_L("Relaying feature reports (challenge-response / slot configuration), total %lu", "フィーチャーレポート中継中（チャレンジレスポンス / スロット設定）累計 %lu 回"),
                         (unsigned long)s_ctl_count);
            }
        }
        if (s_get_pending && now_ms() - s_get_t0 > GET_TIMEOUT_MS) {
            ESP_LOGW(TAG, YD_L("No GET_REPORT reply from the desk unit: STALLing", "机上ユニットから GET_REPORT の返事がないので STALL で返します"));
            usbd_defer_func(get_finish, (void *)(uintptr_t)s_get_tag, false);
            s_get_t0 = now_ms();   /* get_finish が処理するまで連打しない */
        }
    }
}

/* ======================================================================
 *  外部 API
 * ====================================================================== */
void hid_dev_set_otp_caps(bool kbd, bool ctl)
{
    if (s_kbd_on && !kbd) {
        kbd_rep_t z = { 0 };
        xQueueSend(s_kbd_q, &z, 0);   /* 打っている途中で切れてもキーが押しっぱなしにならないように */
    }
    s_kbd_on = kbd;
    s_ctl_on = ctl;
}

void hid_dev_kbd_from_link(const uint8_t *data, uint16_t len)
{
    if (!s_kbd_on || len < KBD_REPORT_LEN || len % KBD_REPORT_LEN) {
        return;                         /* 机上ユニットが OTP キーボードありと言っていないのに届いた分も捨てる */
    }
    for (uint16_t off = 0; off < len; off += KBD_REPORT_LEN) {
        kbd_rep_t r;
        memcpy(r.d, data + off, KBD_REPORT_LEN);
#if CONFIG_YD_OTP_KBD_FILTER_MODHEX
        static kbdf_t f;                /* rx_task だけが触る */
        static uint32_t seen_epoch;
        uint32_t epoch = s_kbd_epoch;
        if (epoch != seen_epoch) {
            seen_epoch = epoch;
            kbdf_drop(&f);              /* kbd_task が送れなかった文字がある */
        }
        int key = kbdf_key(&f, r.d, now_ms());
        if (key == KBDF_BLOCKED) {
            static int64_t last_log;
            s_kbd_blocked++;
            if (now_ms() - last_log > 2000) {
                last_log = now_ms();
                ESP_LOGW(TAG, YD_L("Blocked key input that does not look like a Yubico OTP (modifiers 0x%02x, key 0x%02x; %" PRIu32 " so far). "
                                   "For static passwords or OATH-HOTP, build with CONFIG_YD_OTP_KBD_FILTER_NONE",
                                   "Yubico OTP の形でないキー入力を止めました（修飾キー 0x%02x、キー 0x%02x、累計 %" PRIu32 " 件）。"
                                   "静的パスワードや OATH-HOTP を使うなら CONFIG_YD_OTP_KBD_FILTER_NONE でビルドしてください"),
                         r.d[0], r.d[2], s_kbd_blocked);
            }
            continue;
        }
        if (key == 0) {
            continue;                   /* 「離す」はこちらで送るので、机上ユニットの分は使わない */
        }
        /* 1文字を「押す」「全部離す」の2つで打つ。2つとも入らないなら、押すほうも入れない。
         * rx_task（BLE 受信）を止めないように待たない。捨てた文字もフィルターは数えているので、
         * このまとまりは間が空くまで打たない（空いてから Enter だけが通らないように） */
        if (uxQueueSpacesAvailable(s_kbd_q) < 2) {
            ESP_LOGW(TAG, YD_L("Keyboard queue full: input dropped", "キーボードのキューが一杯。入力を破棄"));
            kbdf_drop(&f);
            break;
        }
        kbd_rep_t press = { .d = { 0, 0, (uint8_t)key }, .epoch = epoch }, up = { .epoch = epoch };
        xQueueSend(s_kbd_q, &press, 0);
        xQueueSend(s_kbd_q, &up, 0);
#else
        bool zero = kbd_all_zero(r.d);
        if (zero && s_kbd_q_zero) {
            continue;                   /* 机上ユニットが長さをそろえるために入れた埋め草 */
        }
        /* rx_task（BLE 受信）を止めないように待たない。あふれた分は捨てる（押したままは KBD_STUCK_MS で離す） */
        s_kbd_q_zero = zero;
        if (xQueueSend(s_kbd_q, &r, 0) != pdTRUE) {
            ESP_LOGW(TAG, YD_L("Keyboard queue full: input dropped", "キーボードのキューが一杯。入力を破棄"));
            break;
        }
#endif
    }
}

void hid_dev_ctl_from_link(const uint8_t *data, uint16_t len)
{
    if (len != 3 + YD_OTPCTL_REPORT_LEN || data[0] != YD_OTPCTL_GET_RESP) {
        return;
    }
    uint8_t tag = data[1];
    bool ok = data[2] != 0;
    if (!s_get_pending || tag != s_get_tag) {
        return;   /* 遅れて届いた古い返事 */
    }
    memcpy(s_get_buf, &data[3], YD_OTPCTL_REPORT_LEN);
    usbd_defer_func(get_finish, (void *)(uintptr_t)(tag | (ok ? 0x100 : 0)), false);
}

void hid_dev_init(void)
{
    s_in_sem = xSemaphoreCreateBinary();
    s_fido_sem = xSemaphoreCreateBinary();
    configASSERT(s_fido_sem);
    s_kbd_q = xQueueCreate(128, sizeof(kbd_rep_t));
    s_ctl_q = xQueueCreate(32, sizeof(ctl_req_t));
    configASSERT(s_in_sem && s_kbd_q && s_ctl_q);
    xTaskCreate(kbd_task, "kbd", 3072, NULL, 11, NULL);
    xTaskCreate(ctl_task, "otp_ctl", 3072, NULL, 10, &s_ctl_task);
}
