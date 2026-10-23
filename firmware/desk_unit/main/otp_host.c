/*
 * otp_host: YubiKey キーボード（OTP）→ レシーバー
 *
 * YubiKey をタッチすると、キーボードとして「押す / 離す」の8バイトのレポートを連続で送ってくる
 * （ブートプロトコルの形: [修飾キー, 0, キー1..キー6]）。
 *
 * BLE の遅れで「押す」と「離す」の間が空くと、Mac がキーリピートを始めて同じ文字が何個も入ってしまう。
 * そこで「全部のキーが離された」レポートが来るまで手元に溜め、押す〜離すをひとまとめにして送る。
 * レシーバーはまとまりの中を USB の速さで続けて流すので、キーを押したまま BLE を待つことがない。
 *
 *   YD_CH_OTP のペイロード = レポート(8バイト) × N
 *
 * ---- フィーチャーレポート（v7） ----
 * チャレンジレスポンス（KeePassXC など）やスロット設定（ykman otp）は、キーボードの口への
 * GET_REPORT / SET_REPORT（Feature, 8バイト）で行う。レシーバーが Mac から受けた要求を1つずつ送ってくるので、
 * 制御転送でそのまま YubiKey に出し、GET の結果を返す（中身は解釈しない）。
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
#include "otp_host.h"
#include "yd_lang.h"

static const char *TAG = "otp";

#define REPORT_LEN     8
#define BATCH_MAX      24          /* 1回で送るレポート数の上限（8×24 = 192バイト） */
#define PRESS_FLUSH_MS 100         /* 押したままのレポートがこれだけ溜まっていたら、離されなくても送る */

#define CTL_TIMEOUT_MS 1000

typedef struct {
    uint8_t op, tag;
    uint8_t d[YD_OTPCTL_REPORT_LEN];
} ctl_req_t;

static QueueHandle_t s_ctl_q;
static SemaphoreHandle_t s_ctl_done, s_ctl_mutex;
static usb_transfer_t *s_xctl;
static uint32_t s_ctl_count;

static struct {
    usb_host_client_handle_t client;
    usb_device_handle_t dev;
    int itf;
    uint8_t ep;
    uint16_t mps;
    usb_transfer_t *xin;
    volatile bool in_flight;
    volatile bool attached;
} o = { .itf = -1 };

/* usb_client タスクだけが触る */
static uint8_t s_batch[BATCH_MAX * REPORT_LEN];
static int s_nrep;
static int64_t s_first_us;
static uint32_t s_chars;

static bool keys_released(const uint8_t *r)
{
    for (int i = 2; i < REPORT_LEN; i++) {
        if (r[i]) {
            return false;
        }
    }
    return true;
}

static void flush(void)
{
    if (s_nrep == 0) {
        return;
    }
    esp_err_t err = yd_link_send(YD_CH_OTP, s_batch, (uint16_t)(s_nrep * REPORT_LEN));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, YD_L("Send to the receiver failed (%s). Dropped %d key reports", "レシーバーへの送信失敗 (%s)。キー入力 %d 件を破棄"), esp_err_to_name(err), s_nrep);
    }
    s_nrep = 0;
}

static void in_cb(usb_transfer_t *x)
{
    if (x->status == USB_TRANSFER_STATUS_COMPLETED && x->actual_num_bytes >= REPORT_LEN) {
        const uint8_t *r = x->data_buffer;
        if (s_nrep == 0) {
            s_first_us = esp_timer_get_time();
        }
        memcpy(&s_batch[s_nrep * REPORT_LEN], r, REPORT_LEN);
        s_nrep++;
        bool released = keys_released(r);
        if (!released) {
            s_chars++;
        }
        /* 離されたら送る。溜まりすぎ・押したまま長すぎ（異常時）も送る */
        if (released || s_nrep >= BATCH_MAX ||
            esp_timer_get_time() - s_first_us > (int64_t)PRESS_FLUSH_MS * 1000) {
            flush();
        }
        if (released && s_chars) {
            static int64_t last_log;
            int64_t t = esp_timer_get_time();
            if (t - last_log > 2000000) {   /* 1回のタッチで1行だけ */
                last_log = t;
                ESP_LOGI(TAG, YD_L("Key started typing (touch) → relaying to the host", "YubiKeyがキー入力を開始（タッチ）→ Macへ中継"));
            }
        }
    } else if (x->status != USB_TRANSFER_STATUS_COMPLETED && x->status != USB_TRANSFER_STATUS_CANCELED &&
               x->status != USB_TRANSFER_STATUS_NO_DEVICE) {
        ESP_LOGW(TAG, "interrupt IN status=%d", x->status);
    }
    if (o.attached && (x->status == USB_TRANSFER_STATUS_COMPLETED)) {
        x->num_bytes = o.mps;
        if (usb_host_transfer_submit(x) == ESP_OK) {
            return;
        }
    }
    o.in_flight = false;
}

bool otph_attach(usb_host_client_handle_t client, usb_device_handle_t dev, const usb_config_desc_t *cd,
                 const usb_intf_desc_t *itf, int itf_offset, bool verbose)
{
    if (o.attached) {
        return true;
    }
    uint8_t ep_in = 0;
    uint16_t mps = 0;
    for (int e = 0; e < itf->bNumEndpoints; e++) {
        int off = itf_offset;
        const usb_ep_desc_t *ep = usb_parse_endpoint_descriptor_by_index(itf, e, cd->wTotalLength, &off);
        if (ep && (ep->bEndpointAddress & 0x80) && USB_EP_DESC_GET_XFERTYPE(ep) == USB_TRANSFER_TYPE_INTR) {
            ep_in = ep->bEndpointAddress;
            mps = USB_EP_DESC_GET_MPS(ep);
        }
    }
    if (!ep_in || mps < REPORT_LEN || mps > 64) {
        ESP_LOGW(TAG, YD_L("Keyboard interrupt IN endpoint not found", "キーボードの interrupt IN が見つかりません"));
        return false;
    }
    esp_err_t err = usb_host_interface_claim(client, dev, itf->bInterfaceNumber, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, YD_L("Failed to claim the keyboard interface: %s", "キーボードインターフェースの取得に失敗: %s"), esp_err_to_name(err));
        return false;
    }
    o.client = client;
    o.dev = dev;
    o.itf = itf->bInterfaceNumber;
    o.ep = ep_in;
    o.mps = mps;
    s_nrep = 0;
    s_chars = 0;
    o.xin->device_handle = dev;
    o.xin->bEndpointAddress = ep_in;
    o.xin->callback = in_cb;
    o.xin->num_bytes = mps;
    o.attached = true;
    o.in_flight = true;
    if (usb_host_transfer_submit(o.xin) != ESP_OK) {
        o.in_flight = false;
        ESP_LOGW(TAG, YD_L("Could not start reading the keyboard", "キーボードの読み取りを開始できませんでした"));
    }
    if (verbose) {
        ESP_LOGI(TAG, YD_L("→ Claimed keyboard (OTP): itf%u EP IN=0x%02x MPS=%u", "→ キーボード（OTP）を取得: itf%u EP IN=0x%02x MPS=%u"), o.itf, ep_in, mps);
    }
    return true;
}

void otph_detach(void)
{
    if (o.itf < 0) {
        return;
    }
    xSemaphoreTake(s_ctl_mutex, portMAX_DELAY);   /* 制御転送の途中なら終わるのを待つ */
    o.attached = false;
    xSemaphoreGive(s_ctl_mutex);
    usb_host_endpoint_halt(o.dev, o.ep);
    usb_host_endpoint_flush(o.dev, o.ep);
    for (int i = 0; i < 100 && o.in_flight; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    usb_host_endpoint_clear(o.dev, o.ep);
    usb_host_interface_release(o.client, o.dev, o.itf);
    o.itf = -1;
    o.dev = NULL;
    s_nrep = 0;
}

bool otph_present(void)
{
    return o.attached;
}

/* ======================================================================
 *  フィーチャーレポート（制御転送）
 * ====================================================================== */
static void ctl_cb(usb_transfer_t *x)
{
    xSemaphoreGive(s_ctl_done);
}

/* is_get: GET_REPORT(Feature) / それ以外: SET_REPORT(Feature)。s_ctl_mutex を持って呼ぶ */
static bool ctl_xfer(bool is_get, uint8_t *data)
{
    if (!o.attached) {
        return false;
    }
    usb_setup_packet_t *sp = (usb_setup_packet_t *)s_xctl->data_buffer;
    sp->bmRequestType = (is_get ? USB_BM_REQUEST_TYPE_DIR_IN : USB_BM_REQUEST_TYPE_DIR_OUT) |
                        USB_BM_REQUEST_TYPE_TYPE_CLASS | USB_BM_REQUEST_TYPE_RECIP_INTERFACE;
    sp->bRequest = is_get ? 0x01 : 0x09;          /* GET_REPORT / SET_REPORT */
    sp->wValue = (0x03 << 8) | 0x00;              /* Feature, Report ID 0 */
    sp->wIndex = o.itf;
    sp->wLength = YD_OTPCTL_REPORT_LEN;
    if (!is_get) {
        memcpy(s_xctl->data_buffer + sizeof(usb_setup_packet_t), data, YD_OTPCTL_REPORT_LEN);
    }
    s_xctl->num_bytes = sizeof(usb_setup_packet_t) + YD_OTPCTL_REPORT_LEN;
    s_xctl->device_handle = o.dev;
    s_xctl->bEndpointAddress = 0;
    s_xctl->callback = ctl_cb;
    s_xctl->timeout_ms = CTL_TIMEOUT_MS;
    xSemaphoreTake(s_ctl_done, 0);
    if (usb_host_transfer_submit_control(o.client, s_xctl) != ESP_OK) {
        return false;
    }
    if (xSemaphoreTake(s_ctl_done, pdMS_TO_TICKS(CTL_TIMEOUT_MS + 200)) != pdTRUE) {
        ESP_LOGW(TAG, YD_L("Feature report control transfer timed out", "フィーチャーレポートの制御転送がタイムアウト"));
        return false;
    }
    if (s_xctl->status != USB_TRANSFER_STATUS_COMPLETED) {
        return false;
    }
    if (is_get) {
        if (s_xctl->actual_num_bytes < (int)(sizeof(usb_setup_packet_t) + YD_OTPCTL_REPORT_LEN)) {
            return false;
        }
        memcpy(data, s_xctl->data_buffer + sizeof(usb_setup_packet_t), YD_OTPCTL_REPORT_LEN);
    }
    return true;
}

static void ctl_task(void *arg)
{
    ctl_req_t r;
    int64_t last_log = 0;
    for (;;) {
        xQueueReceive(s_ctl_q, &r, portMAX_DELAY);
        xSemaphoreTake(s_ctl_mutex, portMAX_DELAY);
        bool ok;
        if (r.op == YD_OTPCTL_SET) {
            ok = ctl_xfer(false, r.d);
            if (!ok) {
                ESP_LOGW(TAG, YD_L("SET_REPORT(Feature) failed", "SET_REPORT(Feature) 失敗"));
            }
        } else {
            uint8_t d[YD_OTPCTL_REPORT_LEN] = { 0 };
            ok = ctl_xfer(true, d);
            uint8_t resp[3 + YD_OTPCTL_REPORT_LEN] = { YD_OTPCTL_GET_RESP, r.tag, ok ? 1 : 0 };
            memcpy(&resp[3], d, YD_OTPCTL_REPORT_LEN);
            yd_link_send(YD_CH_OTPCTL, resp, sizeof(resp));
        }
        xSemaphoreGive(s_ctl_mutex);
        s_ctl_count++;
        int64_t t = esp_timer_get_time();
        if (t - last_log > 3000000) {   /* やり取りが続いている間は3秒に1行 */
            last_log = t;
            ESP_LOGI(TAG, YD_L("Relaying feature reports (challenge-response / slot configuration), total %", "フィーチャーレポート中継中（チャレンジレスポンス / スロット設定）累計 %") PRIu32 YD_L("", " 回"), s_ctl_count);
        }
    }
}

void otph_ctl_from_link(const uint8_t *data, uint16_t len)
{
    ctl_req_t r = { 0 };
    if (len == 1 + YD_OTPCTL_REPORT_LEN && data[0] == YD_OTPCTL_SET) {
        r.op = YD_OTPCTL_SET;
        memcpy(r.d, data + 1, YD_OTPCTL_REPORT_LEN);
    } else if (len == 2 && data[0] == YD_OTPCTL_GET) {
        r.op = YD_OTPCTL_GET;
        r.tag = data[1];
    } else {
        return;
    }
    if (xQueueSend(s_ctl_q, &r, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, YD_L("Feature report queue full", "フィーチャーレポートのキューが一杯"));
    }
}

void otph_init(void)
{
    ESP_ERROR_CHECK(usb_host_transfer_alloc(64, 0, &o.xin));
    ESP_ERROR_CHECK(usb_host_transfer_alloc(sizeof(usb_setup_packet_t) + 64, 0, &s_xctl));
    s_ctl_q = xQueueCreate(32, sizeof(ctl_req_t));
    s_ctl_done = xSemaphoreCreateBinary();
    s_ctl_mutex = xSemaphoreCreateMutex();
    configASSERT(s_ctl_q && s_ctl_done && s_ctl_mutex);
    xTaskCreatePinnedToCore(ctl_task, "otp_ctl", 3072, NULL, 8, NULL, 0);
}
