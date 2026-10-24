#include <string.h>
#include "yd_link_priv.h"
#include "yd_frag.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_check.h"

static const char *TAG = "yd_link";

yd_link_stats_t g_ydl_stats;

#define SYNC0 0xA5
#define SYNC1 0x5A
#define HDR_LEN 5          /* sync0 sync1 ch lenL lenH */
#define CRC_LEN 2
#define UART_BUF_SIZE 4096

static yd_link_config_t s_cfg;
static SemaphoreHandle_t s_tx_mutex;
static uint8_t s_tx_buf[HDR_LEN + YD_LINK_MAX_PAYLOAD + CRC_LEN];

static uint16_t crc16_ccitt(uint16_t crc, const uint8_t *p, size_t n)
{
    while (n--) {
        crc ^= (uint16_t)(*p++) << 8;
        for (int i = 0; i < 8; i++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

esp_err_t ydl_uart_send(uint8_t ch, const uint8_t *data, uint16_t len)
{
    ESP_RETURN_ON_FALSE(len <= YD_LINK_MAX_PAYLOAD, ESP_ERR_INVALID_SIZE, TAG, "payload too large");
    ESP_RETURN_ON_FALSE(s_tx_mutex, ESP_ERR_INVALID_STATE, TAG, "not initialized");

    xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
    uint8_t *b = s_tx_buf;
    b[0] = SYNC0;
    b[1] = SYNC1;
    b[2] = ch;
    b[3] = (uint8_t)(len & 0xFF);
    b[4] = (uint8_t)(len >> 8);
    if (len) {
        memcpy(&b[HDR_LEN], data, len);
    }
    uint16_t crc = crc16_ccitt(0xFFFF, &b[2], 3 + len);
    b[HDR_LEN + len] = (uint8_t)(crc & 0xFF);
    b[HDR_LEN + len + 1] = (uint8_t)(crc >> 8);
    int w = uart_write_bytes(s_cfg.uart_num, b, HDR_LEN + len + CRC_LEN);
    g_ydl_stats.tx_frames++;
    xSemaphoreGive(s_tx_mutex);
    return (w == HDR_LEN + len + CRC_LEN) ? ESP_OK : ESP_FAIL;
}


/* 受信ステートマシン */
typedef enum { ST_SYNC0, ST_SYNC1, ST_CH, ST_LEN0, ST_LEN1, ST_PAYLOAD, ST_CRC0, ST_CRC1 } rx_state_t;

static void rx_task(void *arg)
{
    static uint8_t payload[YD_LINK_MAX_PAYLOAD];
    uint8_t chunk[256];
    rx_state_t st = ST_SYNC0;
    uint8_t ch = 0;
    uint16_t len = 0, pos = 0, crc_rx = 0;

    for (;;) {
        int n = uart_read_bytes(s_cfg.uart_num, chunk, sizeof(chunk), pdMS_TO_TICKS(20));
        for (int i = 0; i < n; i++) {
            uint8_t c = chunk[i];
            switch (st) {
            case ST_SYNC0: st = (c == SYNC0) ? ST_SYNC1 : ST_SYNC0; break;
            case ST_SYNC1: st = (c == SYNC1) ? ST_CH : ((c == SYNC0) ? ST_SYNC1 : ST_SYNC0); break;
            case ST_CH:    ch = c; st = ST_LEN0; break;
            case ST_LEN0:  len = c; st = ST_LEN1; break;
            case ST_LEN1:
                len |= (uint16_t)c << 8;
                pos = 0;
                if (len > YD_LINK_MAX_PAYLOAD) {
                    g_ydl_stats.rx_overflows++;
                    st = ST_SYNC0;
                } else {
                    st = len ? ST_PAYLOAD : ST_CRC0;
                }
                break;
            case ST_PAYLOAD:
                payload[pos++] = c;
                if (pos == len) {
                    st = ST_CRC0;
                }
                break;
            case ST_CRC0: crc_rx = c; st = ST_CRC1; break;
            case ST_CRC1: {
                crc_rx |= (uint16_t)c << 8;
                uint8_t hdr[3] = { ch, (uint8_t)(len & 0xFF), (uint8_t)(len >> 8) };
                uint16_t crc = crc16_ccitt(0xFFFF, hdr, 3);
                crc = crc16_ccitt(crc, payload, len);
                if (crc == crc_rx) {
                    g_ydl_stats.rx_frames++;
                    if (s_cfg.rx_cb) {
                        s_cfg.rx_cb(ch, payload, len, s_cfg.rx_cb_arg);
                    }
                } else {
                    g_ydl_stats.rx_crc_errors++;
                    ESP_LOGW(TAG, "CRC error (ch=0x%02x len=%u)", ch, len);
                }
                st = ST_SYNC0;
                break;
            }
            }
        }
    }
}

esp_err_t ydl_uart_init(const yd_link_config_t *cfg)
{
    s_cfg = *cfg;
    uart_config_t uc = {
        .baud_rate = cfg->baudrate,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(uart_driver_install(cfg->uart_num, UART_BUF_SIZE, UART_BUF_SIZE, 0, NULL, 0), TAG, "uart install");
    ESP_RETURN_ON_ERROR(uart_param_config(cfg->uart_num, &uc), TAG, "uart param");
    ESP_RETURN_ON_ERROR(uart_set_pin(cfg->uart_num, cfg->tx_gpio, cfg->rx_gpio, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE), TAG, "uart pin");

    s_tx_mutex = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_tx_mutex, ESP_ERR_NO_MEM, TAG, "mutex");
    BaseType_t ok = xTaskCreate(rx_task, "yd_link_rx", 4096, NULL, 12, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "rx task");
    ESP_LOGI(TAG, "link up: UART%d TX=GPIO%d RX=GPIO%d %d baud", cfg->uart_num, cfg->tx_gpio, cfg->rx_gpio, cfg->baudrate);
    return ESP_OK;
}

/* ======================================================================
 *  公開API（トランスポートに振り分け）
 * ====================================================================== */
static yd_link_transport_t s_transport;

esp_err_t yd_link_init(const yd_link_config_t *cfg)
{
    s_transport = cfg->transport;
    yd_frag_init();
    switch (cfg->transport) {
    case YD_LINK_UART:
        return ydl_uart_init(cfg);
    case YD_LINK_BLE_PERIPHERAL:
    case YD_LINK_BLE_CENTRAL:
        return ydl_ble_init(cfg);
    }
    return ESP_ERR_INVALID_ARG;
}

esp_err_t yd_link_send(uint8_t ch, const uint8_t *data, uint16_t len)
{
    return (s_transport == YD_LINK_UART) ? ydl_uart_send(ch, data, len) : ydl_ble_send(ch, data, len);
}

esp_err_t yd_link_send_on(uint32_t conn, uint8_t ch, const uint8_t *data, uint16_t len)
{
    return (s_transport == YD_LINK_UART) ? ydl_uart_send(ch, data, len) : ydl_ble_send_on(conn, ch, data, len);
}

uint16_t yd_link_max_payload(void)
{
    return (s_transport == YD_LINK_UART) ? YD_LINK_MAX_PAYLOAD : ydl_ble_max_payload();
}

bool yd_link_is_up(void)
{
    return (s_transport == YD_LINK_UART) ? true : ydl_ble_is_up();
}

uint32_t yd_link_conn_id(void)
{
    return (s_transport == YD_LINK_UART) ? 0 : ydl_ble_conn_id();
}

esp_err_t yd_link_forget_peers(void)
{
    return (s_transport == YD_LINK_UART) ? ESP_OK : ydl_ble_forget_peers();
}

void yd_link_get_stats(yd_link_stats_t *out)
{
    *out = g_ydl_stats;
}

void yd_link_get_conn_info(yd_link_conn_info_t *out)
{
    memset(out, 0, sizeof(*out));
    if (s_transport == YD_LINK_UART) {
        out->connected = true;
        return;
    }
    ydl_ble_get_conn_info(out);
}
