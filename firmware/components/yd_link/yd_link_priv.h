#pragma once
#include "yd_link.h"

extern yd_link_stats_t g_ydl_stats;

esp_err_t ydl_uart_init(const yd_link_config_t *cfg);
esp_err_t ydl_uart_send(uint8_t ch, const uint8_t *data, uint16_t len);

esp_err_t ydl_ble_init(const yd_link_config_t *cfg);
esp_err_t ydl_ble_send(uint8_t ch, const uint8_t *data, uint16_t len);
esp_err_t ydl_ble_send_on(uint32_t conn, uint8_t ch, const uint8_t *data, uint16_t len);
bool ydl_ble_is_up(void);
uint32_t ydl_ble_conn_id(void);
uint16_t ydl_ble_max_payload(void);
esp_err_t ydl_ble_forget_peers(void);
void ydl_ble_get_conn_info(yd_link_conn_info_t *out);
