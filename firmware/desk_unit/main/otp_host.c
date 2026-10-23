/*
 * otp_host: YubiKey キーボード（OTP）→ レシーバー
 *
 * YubiKey をタッチすると、キーボードとして「押す / 離す」の8バイトのレポートを連続で送ってくる
 * （ブートプロトコルの形: [修飾キー, 0, キー1..キー6]）。
 *
 * BLE の遅れで「押す」と「離す」の間が空くと、Mac がキーリピートを始めて同じ文字が何個も入ってしまう。
 * そこでレポートを手元に溜め、「離した」ところで区切って送る（otp_batch.h）。レシーバーはまとまりの中を
 * USB の速さで続けて流すので、キーを押したまま BLE を待つことがない。
 *
 * 電波から文字数（静的パスワードの長さなど）が分からないように、タッチ1回分は「決まった間隔・決まった数・
 * 同じ大きさ」のパケット列で送る。最初のレポートが来たら、TICK_MS ごとに1パケット、少なくとも
 * SESSION_TICKS 回送る。中身は、その時点で溜まっている分（足りない分は「全部離す」のレポートで埋める。
 * レシーバーは続く「全部離す」を1つにまとめる）。打ち終わるのが遅れる（キーの pacing 設定など）と
 * そのぶん続け、そのときだけ長さが見える。送るのは otp_tx タスクだけなので、順番は入れ替わらない。
 *
 *   YD_CH_OTP のペイロード = レポート(8バイト) × rpp（MTU から決まる。247 なら 24）
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
#include "otp_batch.h"
#include "yd_lang.h"

static const char *TAG = "otp";

#define REPORT_LEN      OTPB_REPORT_LEN
#define RPP_MAX         24          /* 1パケットのレポート数の上限（8×24 = 192バイト） */
#define TICK_MS         50          /* タッチ1回分のパケットを送る間隔 */
#define SESSION_TICKS   40          /* 少なくともこの回数送る（50ms×40 = 2秒。Yubico OTP の入力より長い） */
#define IDLE_END_MS     150         /* 最後のレポートからこれだけ空いたら入力が終わったとみなす（pacing 60ms より長く） */
#define PRESS_FORCE_MS  300         /* 押したまま、これだけ離されなければ（異常時）途中まで送る */

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

/* in_cb（usb_client タスク）が足し、otp_tx タスクが取り出す。s_ring_mutex で守る */
static otpb_ring_t s_ring;
static bool s_session;              /* otp_tx タスクがパケット列を送っている */
static bool s_last_released = true;
static bool s_last_zero = true;     /* 最後に溜めたレポートが全部 0（何も押していない） */
static int64_t s_last_us;
static uint32_t s_chars, s_overflow;
static SemaphoreHandle_t s_ring_mutex;
static TaskHandle_t s_tx_task;

/* タッチ1回分を、決まった間隔・数・大きさのパケット列で送る */
static void otp_tx_task(void *arg)
{
    static uint8_t pkt[RPP_MAX * REPORT_LEN];
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);          /* 入力の始まり */
        TickType_t wake = xTaskGetTickCount();
        int ticks = 0;
        bool warned = false;
        for (;;) {
            vTaskDelayUntil(&wake, pdMS_TO_TICKS(TICK_MS));
            ticks++;
            int rpp = yd_link_max_payload() / REPORT_LEN;
            if (rpp > RPP_MAX) {
                rpp = RPP_MAX;
            }
            memset(pkt, 0, sizeof(pkt));
            xSemaphoreTake(s_ring_mutex, portMAX_DELAY);
            int64_t idle_ms = (esp_timer_get_time() - s_last_us) / 1000;
            int n = rpp > 0 ? otpb_take(&s_ring, pkt, rpp, !s_last_released && idle_ms > PRESS_FORCE_MS) : 0;
            bool done = ticks >= SESSION_TICKS && s_ring.count == 0 && idle_ms >= IDLE_END_MS;
            if (done) {
                s_session = false;        /* in_cb は同じ mutex の中でこれを見るので、取り残しは出ない */
            }
            if (rpp <= 0 && s_ring.count) {
                otpb_init(&s_ring);       /* リンクがない。溜めても送れないので捨てる */
            }
            xSemaphoreGive(s_ring_mutex);
            if (rpp > 0) {
                esp_err_t err = ESP_FAIL;
                for (int retry = 0; retry < 5; retry++) {
                    err = yd_link_send(YD_CH_OTP, pkt, (uint16_t)(rpp * REPORT_LEN));
                    if (err != ESP_ERR_TIMEOUT) {
                        break;
                    }
                }
                if (err != ESP_OK && n > 0) {
                    ESP_LOGW(TAG, YD_L("Send to the receiver failed (%s). Dropped %d key reports", "レシーバーへの送信失敗 (%s)。キー入力 %d 件を破棄"), esp_err_to_name(err), n);
                }
            } else if (!warned) {
                warned = true;
                ESP_LOGW(TAG, YD_L("No link to the receiver: key input dropped", "レシーバーとつながっていないので、キー入力を捨てました"));
            }
            if (done) {
                break;
            }
        }
    }
}

static void in_cb(usb_transfer_t *x)
{
    if (x->status == USB_TRANSFER_STATUS_COMPLETED && x->actual_num_bytes >= REPORT_LEN) {
        const uint8_t *r = x->data_buffer;
        bool released = otpb_released(r);
        bool zero = released && r[0] == 0 && r[1] == 0;
        xSemaphoreTake(s_ring_mutex, portMAX_DELAY);
        bool redundant = zero && s_last_zero;  /* 何も押していないところに「全部 0」。送らなくてよい */
        bool ok = redundant || otpb_push(&s_ring, r);
        if (ok && !redundant) {
            s_last_released = released;
            s_last_zero = zero;
            s_last_us = esp_timer_get_time();
        } else if (!ok) {
            s_overflow++;
        }
        bool start = ok && !redundant && !s_session;
        if (start) {
            s_session = true;
        }
        xSemaphoreGive(s_ring_mutex);
        if (start) {
            xTaskNotifyGive(s_tx_task);
        }
        if (!ok) {
            ESP_LOGW(TAG, YD_L("Key input buffer full: report dropped (%" PRIu32 ")", "キー入力のバッファが一杯でレポートを捨てました（%" PRIu32 "）"), s_overflow);
        }
        if (!released) {
            s_chars++;
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
    xSemaphoreTake(s_ring_mutex, portMAX_DELAY);
    otpb_init(&s_ring);
    s_last_released = s_last_zero = true;
    xSemaphoreGive(s_ring_mutex);
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
    xSemaphoreTake(s_ring_mutex, portMAX_DELAY);
    otpb_init(&s_ring);               /* 送っている途中なら、残りは「全部離す」だけのパケットになる */
    s_last_released = s_last_zero = true;
    xSemaphoreGive(s_ring_mutex);
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
    s_ring_mutex = xSemaphoreCreateMutex();
    configASSERT(s_ctl_q && s_ctl_done && s_ctl_mutex && s_ring_mutex);
    otpb_init(&s_ring);
    xTaskCreatePinnedToCore(otp_tx_task, "otp_tx", 3072, NULL, 9, &s_tx_task, 0);
    xTaskCreatePinnedToCore(ctl_task, "otp_ctl", 3072, NULL, 8, NULL, 0);
}
