/*
 * otp_host: YubiKey のキーボード（OTP）インターフェースを読み、タッチで打たれたキー入力をレシーバーへ送る
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "usb/usb_host.h"

void otph_init(void);

/* open_device から（s_bus_mutex を持って）。itf_offset は usb_parse_next_descriptor_of_type が返した位置 */
bool otph_attach(usb_host_client_handle_t client, usb_device_handle_t dev, const usb_config_desc_t *cd,
                 const usb_intf_desc_t *itf, int itf_offset, bool verbose);

/* close_device から（デバイスを閉じる前に） */
void otph_detach(void);

bool otph_present(void);

/* レシーバーから届いた YD_CH_OTPCTL（yd_link 受信タスク） */
void otph_ctl_from_link(const uint8_t *data, uint16_t len);
