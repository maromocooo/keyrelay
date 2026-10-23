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
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "yd_link.h"
#include "hid_dev.h"

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

/* ---- 保留中の GET_REPORT(Feature)（TinyUSB タスクで書き換え） ---- */
static volatile bool s_get_pending;
static volatile uint8_t s_get_tag;
static volatile int64_t s_get_t0;

/* ---- キュー ---- */
typedef struct {
    uint8_t d[KBD_REPORT_LEN];
} kbd_rep_t;

typedef struct {
    uint8_t op, tag;
    uint8_t d[YD_OTPCTL_REPORT_LEN];
} ctl_req_t;

static QueueHandle_t s_kbd_q, s_ctl_q;
static TaskHandle_t s_ctl_task;
static volatile bool s_kbd_on, s_ctl_on;
static uint32_t s_ctl_count;

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
        tusb_control_request_t dummy = { 0 };   /* tud_control_xfer は保存済みの要求を使う */
        tud_control_xfer(0, &dummy, s_get_buf, KBD_REPORT_LEN);
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
        ESP_LOGW(TAG, "フィーチャーレポートのキューが一杯");
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
                    ESP_LOGW(TAG, "FIDO: INレポートの完了通知なし");
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
static bool kbd_send(const uint8_t *d)
{
    TickType_t start = xTaskGetTickCount();
    while ((xTaskGetTickCount() - start) < pdMS_TO_TICKS(500)) {
        if (s_opened && tud_mounted() && usbd_edpt_claim(0, s_ep_in)) {
            memcpy(s_in_buf, d, KBD_REPORT_LEN);
            xSemaphoreTake(s_in_sem, 0);
            if (usbd_edpt_xfer(0, s_ep_in, s_in_buf, KBD_REPORT_LEN, false)) {
                xSemaphoreTake(s_in_sem, pdMS_TO_TICKS(100));
                return true;
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

static void kbd_task(void *arg)
{
    kbd_rep_t r;
    bool pressed = false, first = true;
    TickType_t last = 0;
    for (;;) {
        if (xQueueReceive(s_kbd_q, &r, pressed ? pdMS_TO_TICKS(KBD_STUCK_MS) : portMAX_DELAY) != pdTRUE) {
            static const uint8_t zero[KBD_REPORT_LEN] = { 0 };
            ESP_LOGW(TAG, "キーが押されたまま続きが来ないので離します");
            kbd_send(zero);
            pressed = false;
            continue;
        }
#if YD_OTP_FORCE_EISU
        if (kbd_has_keys(r.d) && (first || (xTaskGetTickCount() - last) > pdMS_TO_TICKS(KBD_BURST_GAP_MS))) {
            static const uint8_t eisu[KBD_REPORT_LEN] = { 0, 0, HID_KEY_LANG2 };
            static const uint8_t up[KBD_REPORT_LEN] = { 0 };
            kbd_send(eisu);
            kbd_send(up);
            vTaskDelay(pdMS_TO_TICKS(KBD_EISU_SETTLE_MS));
            ESP_LOGI(TAG, "OTP入力: 先に「英数」キーを送りました");
        }
#endif
        if (!kbd_send(r.d)) {
            ESP_LOGW(TAG, "キーボードの送信待ちタイムアウト（Macに未接続？）");
        }
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
                ESP_LOGI(TAG, "フィーチャーレポート中継中（チャレンジレスポンス / スロット設定）累計 %lu 回",
                         (unsigned long)s_ctl_count);
            }
        }
        if (s_get_pending && now_ms() - s_get_t0 > GET_TIMEOUT_MS) {
            ESP_LOGW(TAG, "机上ユニットから GET_REPORT の返事がないので STALL で返します");
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
    if (len < KBD_REPORT_LEN || len % KBD_REPORT_LEN) {
        return;
    }
    for (uint16_t off = 0; off < len; off += KBD_REPORT_LEN) {
        kbd_rep_t r;
        memcpy(r.d, data + off, KBD_REPORT_LEN);
        if (xQueueSend(s_kbd_q, &r, pdMS_TO_TICKS(100)) != pdTRUE) {
            ESP_LOGW(TAG, "キーボードのキューが一杯。入力を破棄");
            break;
        }
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
