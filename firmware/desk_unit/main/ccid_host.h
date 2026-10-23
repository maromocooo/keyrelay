/*
 * ccid_host: 机上ユニット側。YubiKey の CCID インターフェース（スマートカード）を USB ホストとして掴み、
 * レシーバーから届いた CCID メッセージを書き込み、YubiKey の応答をそのまま返す。
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "usb/usb_host.h"

void ccidh_init(void);

/* open_device から（s_bus_mutex を持って）。itf_offset は usb_parse_next_descriptor_of_type が返した位置 */
bool ccidh_attach(usb_host_client_handle_t client, usb_device_handle_t dev, const usb_config_desc_t *cd,
                  const usb_intf_desc_t *itf, int itf_offset, bool verbose);

/* close_device から（デバイスを閉じる前に） */
void ccidh_detach(void);

/* CCID を中継できる状態か */
bool ccidh_present(void);

/* レシーバーから届いた YD_CH_CCID の1片（yd_link 受信タスク） */
void ccidh_from_link(const uint8_t *data, uint16_t len);

/* 最後に CCID のやり取りがあった時刻（esp_timer、us） */
int64_t ccidh_last_activity_us(void);
