/*
 * KeyRelay レシーバー
 *
 *   Mac ──USB(デバイス)── ESP32-S3 ──BLE── 机上ユニット ── YubiKey
 *
 * Macからは次の2つを持つUSB機器に見える（Mac側に追加ソフトは不要）:
 *  - itf0: FIDOセキュリティキー（usage page 0xF1D0 の HID）
 *      64バイトのレポートを机上ユニットとそのまま中継
 *  - itf1: スマートカードリーダー（CCID）。PIV / OpenPGP / OATH 用
 *      CCIDメッセージを丸ごと中継（ccid_dev.c）
 *  - itf2: キーボード。YubiKey をタッチしたときの OTP 入力をそのまま打つ
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "ccid_dev.h"
#include "hid_dev.h"
#include "sdkconfig.h"
#include "yd_link.h"
#include "driver/gpio.h"
#include "yd_lang.h"

static const char *TAG = "recv";

/* ---- 配線（README参照） ---- */
#define LINK_UART        1
#define LINK_TX_GPIO     17
#define LINK_RX_GPIO     18
#define LINK_BAUD        921600
/* 1にすると従来の有線UARTで中継（BLEの切り分け用） */
#define YD_USE_UART      0


/* ---- USB の名乗り ----
 * !!! テスト用 PID（pid.codes 0x1209:0x0001）。他の機器と重なりうる「社内テスト専用」の ID で、
 * 作って配ったり売ったりする機器に使ってはいけない（pid.codes の規則）。
 * KeyRelay 用の PID を pid.codes に申請して取れたら、ここを書き換える。
 * Test PID from pid.codes: NOT unique, for private testing only. Do not ship devices with it.
 *
 * 他社（Yubico など）の VID/PID や製品名は名乗らない。ykman・Yubico Authenticator・KeePassXC は
 * Yubico の USB ID の機器しか相手にしないため、これらのツールからは見えない（README 参照） */
#define YD_VID           0x1209
#define YD_PID           0x0001
#define YD_MANUFACTURER  "KeyRelay"
#define YD_PRODUCT       "KeyRelay Receiver"
#define YD_SERIAL_IDX    3

static const tusb_desc_device_t s_dev_desc = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0x00,
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = YD_VID,
    .idProduct = YD_PID,
    .bcdDevice = 0x0070,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = YD_SERIAL_IDX,
    .bNumConfigurations = 0x01,
};

/* CONFIG_YD_OTP_KEYBOARD=n のときはキーボードの口を作らない（FIDO とスマートカードだけ） */
#if CONFIG_YD_OTP_KEYBOARD
#define YD_KBD_ENABLED  1
enum { ITF_NUM_FIDO = 0, ITF_NUM_CCID, ITF_NUM_KBD, ITF_NUM_TOTAL };
#define KBD_DESC_LEN    TUD_OTPKBD_DESC_LEN
#else
#define YD_KBD_ENABLED  0
enum { ITF_NUM_FIDO = 0, ITF_NUM_CCID, ITF_NUM_TOTAL };
#define KBD_DESC_LEN    0
#endif
#define EPNUM_FIDO_OUT  0x01
#define EPNUM_FIDO_IN   0x81
#define EPNUM_CCID_OUT  0x02
#define EPNUM_CCID_IN   0x82
#define EPNUM_CCID_INT  0x83
#define EPNUM_KBD_IN    0x84   /* ESP32-S3 の IN エンドポイントは EP0 込みで5本まで。これで上限 */
#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_FIDOHID_DESC_LEN + TUD_CCID_DESC_LEN + KBD_DESC_LEN)

static const uint8_t s_cfg_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),
    TUD_FIDOHID_DESCRIPTOR(ITF_NUM_FIDO, 4, EPNUM_FIDO_OUT, EPNUM_FIDO_IN),   /* 自前ドライバー（hid_dev.c） */
    TUD_CCID_DESCRIPTOR(ITF_NUM_CCID, 5, EPNUM_CCID_OUT, EPNUM_CCID_IN, EPNUM_CCID_INT),
#if CONFIG_YD_OTP_KEYBOARD
    TUD_OTPKBD_DESCRIPTOR(ITF_NUM_KBD, 6, EPNUM_KBD_IN),   /* 自前ドライバー（hid_dev.c） */
#endif
};
_Static_assert(sizeof(s_cfg_desc) == CONFIG_TOTAL_LEN, "CONFIG_TOTAL_LEN");

static const char *s_strings[] = {
    (const char[]){ 0x09, 0x04 },     /* 0: 言語 = English(US) */
    YD_MANUFACTURER,                  /* 1: Manufacturer */
    YD_PRODUCT,                       /* 2: Product（macOS ではリーダー名にもなる） */
    "000001",                         /* 3: Serial */
    "FIDO relay",                     /* 4: HID interface */
    "KeyRelay CCID",                  /* 5: CCID interface */
    "KeyRelay OTP keyboard",          /* 6: キーボード */
};

/* TinyUSB に追加するクラスドライバー（CCID・HID(FIDO/OTPキーボード)）。TinyUSB 標準の HID は使わない */
static usbd_class_driver_t s_app_drivers[2];

usbd_class_driver_t const *usbd_app_driver_get_cb(uint8_t *driver_count)
{
    s_app_drivers[0] = *ccid_dev_driver();
    s_app_drivers[1] = *hid_dev_driver();
    *driver_count = 2;
    return s_app_drivers;
}

/* ---- 机上ユニット → Mac 方向のキュー ---- */
typedef struct {
    uint8_t data[FIDO_REPORT_LEN];
} report_t;

static QueueHandle_t s_to_host_q;
static volatile bool s_key_attached;

/* ======================================================================
 *  FIDO（Mac → 机上ユニット）
 * ====================================================================== */
static void fido_rx(const uint8_t *rep)
{
    if (rep[4] & 0x80) {
        ESP_LOGI(TAG, YD_L("Host→ CID=%02x%02x%02x%02x CMD=0x%02x", "Mac→ CID=%02x%02x%02x%02x CMD=0x%02x"), rep[0], rep[1], rep[2], rep[3], rep[4]);
    }
    if (!s_key_attached) {
        ESP_LOGW(TAG, YD_L("Request from the host (CMD=0x%02x) but no key attached", "Macから要求あり (CMD=0x%02x) だがYubiKey未接続"), rep[4]);
    }
    yd_link_send(YD_CH_FIDO, rep, FIDO_REPORT_LEN);
}

/* ======================================================================
 *  UART → Mac 方向
 * ====================================================================== */
static void link_rx(uint8_t ch, const uint8_t *data, uint16_t len, void *arg)
{
    if (ch == YD_CH_CCID) {
        ccid_dev_from_link(data, len);
    } else if (ch == YD_CH_OTP) {
#if CONFIG_YD_OTP_KEYBOARD
        hid_dev_kbd_from_link(data, len);
#endif
    } else if (ch == YD_CH_OTPCTL) {
        hid_dev_ctl_from_link(data, len);
    } else if (ch == YD_CH_FIDO && len == FIDO_REPORT_LEN) {
        report_t r;
        memcpy(r.data, data, FIDO_REPORT_LEN);
        if (xQueueSend(s_to_host_q, &r, pdMS_TO_TICKS(100)) != pdTRUE) {
            ESP_LOGW(TAG, YD_L("to_host queue overflow", "to_host キュー溢れ"));
        }
    } else if (ch == YD_CH_CTRL && len >= 1) {
        switch (data[0]) {
        case YD_CTRL_KEY_ATTACHED: {
            uint8_t caps = len >= 2 ? data[1] : YD_KEY_CAP_FIDO;   /* 旧版の机上ユニットは FIDO のみ */
            s_key_attached = true;
            ESP_LOGI(TAG, YD_L("Desk unit: key attached [%s%s]", "机上ユニット: YubiKey 接続済み [%s%s]"), (caps & YD_KEY_CAP_FIDO) ? "FIDO " : "",
                     (caps & YD_KEY_CAP_CCID) ? "CCID" : "");
            ccid_dev_set_card((caps & YD_KEY_CAP_CCID) != 0);
            hid_dev_set_otp_caps(YD_KBD_ENABLED && (caps & YD_KEY_CAP_OTP) != 0, (caps & YD_KEY_CAP_OTPCTL) != 0);
            if (!YD_KBD_ENABLED) {
                ESP_LOGI(TAG, YD_L("  OTP keyboard disabled in this build (CONFIG_YD_OTP_KEYBOARD=n): touch OTPs are not typed", "  OTP キーボードは無効のビルド（CONFIG_YD_OTP_KEYBOARD=n）。タッチでの OTP 入力はしません"));
            } else if (caps & YD_KEY_CAP_OTP) {
                ESP_LOGI(TAG, YD_L("  Relaying OTP (keyboard) too%s", "  OTP（キーボード）も中継します%s"),
                         (caps & YD_KEY_CAP_OTPCTL) ? YD_L(" (plus challenge-response and slot configuration)", "（チャレンジレスポンス・スロット設定も）") : "");
            }
            break;
        }
        case YD_CTRL_KEY_DETACHED:
            s_key_attached = false;
            ccid_dev_set_card(false);
            hid_dev_set_otp_caps(false, false);
            ESP_LOGI(TAG, YD_L("Desk unit: no key attached", "机上ユニット: YubiKey 未接続"));
            break;
        case YD_CTRL_HELLO:
            ESP_LOGI(TAG, YD_L("Desk unit started", "机上ユニットが起動"));
            break;
        }
    }
}

static void to_host_task(void *arg)
{
    report_t r;
    for (;;) {
        xQueueReceive(s_to_host_q, &r, portMAX_DELAY);

        if (!hid_dev_fido_send(r.data, 2000)) {
            ESP_LOGW(TAG, YD_L("HID send timed out, report dropped (CMD=0x%02x)", "HID送信待ちタイムアウト、レポート破棄 (CMD=0x%02x)"), r.data[4]);
            continue;
        }
        if (r.data[4] & 0x80) {
            ESP_LOGI(TAG, YD_L("→host CMD=0x%02x", "→Mac CMD=0x%02x"), r.data[4]);
        }
    }
}

/* リンク確立 / 切断（BLE） */
static void link_state(bool up, void *arg)
{
    if (up) {
        ESP_LOGI(TAG, YD_L("Connected to the desk unit over BLE", "机上ユニットとBLEで接続しました"));
        yd_link_send_ctrl(YD_CTRL_HELLO);   /* → 机上ユニットがYubiKeyの状態を返す */
    } else {
        s_key_attached = false;
        ccid_dev_set_card(false);
        hid_dev_set_otp_caps(false, false);
        ESP_LOGI(TAG, YD_L("BLE connection to the desk unit lost (reconnecting)", "机上ユニットとのBLE接続が切れました（再接続を試みます）"));
    }
}

/* BOOTボタン3秒長押し: ペアリング情報を消去 */
static void button_task(void *arg)
{
    gpio_config_t btn = {
        .pin_bit_mask = 1ULL << GPIO_NUM_0,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&btn);
    int held_ms = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(50));
        if (gpio_get_level(GPIO_NUM_0) == 0) {
            held_ms += 50;
            if (held_ms == 3000) {
                yd_link_forget_peers();
                ESP_LOGW(TAG, YD_L("Pairing data erased. Erase the desk unit's pairing too, then restart both units", "ペアリング情報を消去しました。机上ユニット側も消去してから、両方を再起動してください"));
            }
        } else {
            held_ms = 0;
        }
    }
}

static void usb_event_cb(tinyusb_event_t *event, void *arg)
{
    if (event->id == TINYUSB_EVENT_ATTACHED) {
        ESP_LOGI(TAG, YD_L("Connected to the host (should appear as a FIDO key, a smart-card reader and a keyboard)", "Macに接続されました（FIDOキー＋スマートカードリーダー＋キーボードとして認識されるはず）"));
    } else if (event->id == TINYUSB_EVENT_DETACHED) {
        ESP_LOGI(TAG, YD_L("Disconnected from the host", "Macから切断"));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, YD_L("KeyRelay receiver starting", "KeyRelay レシーバー 起動"));
    s_to_host_q = xQueueCreate(16, sizeof(report_t));
    xTaskCreate(to_host_task, "to_host", 4096, NULL, 11, NULL);
    ccid_dev_init();
    hid_dev_init();
    hid_dev_set_fido_rx(fido_rx);

    yd_link_config_t lc = {
#if YD_USE_UART
        .transport = YD_LINK_UART,
        .uart_num = LINK_UART, .tx_gpio = LINK_TX_GPIO, .rx_gpio = LINK_RX_GPIO, .baudrate = LINK_BAUD,
#else
        .transport = YD_LINK_BLE_CENTRAL,
#endif
        .rx_cb = link_rx, .rx_cb_arg = NULL,
        .state_cb = link_state, .state_cb_arg = NULL,
    };
    ESP_ERROR_CHECK(yd_link_init(&lc));
    /* UART時: 机上ユニットに起動を知らせる（BLE時は接続確立後に link_state から送る） */
    yd_link_send_ctrl(YD_CTRL_HELLO);
    xTaskCreate(button_task, "button", 3072, NULL, 3, NULL);

    tinyusb_config_t tc = TINYUSB_DEFAULT_CONFIG(usb_event_cb);
    tc.descriptor.device = &s_dev_desc;
    tc.descriptor.string = s_strings;
    tc.descriptor.string_count = sizeof(s_strings) / sizeof(s_strings[0]);
    tc.descriptor.full_speed_config = s_cfg_desc;
    ESP_ERROR_CHECK(tinyusb_driver_install(&tc));
    ESP_LOGI(TAG, YD_L("USB started. Plug the native USB port into the computer", "USB起動。ネイティブUSB側のUSB-CをMacに挿してください"));
}
