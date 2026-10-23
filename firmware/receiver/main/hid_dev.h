/*
 * hid_dev: レシーバーの HID インターフェース2つ（自前の HID クラスドライバー。TinyUSB 標準の HID は使わない）
 *
 *  - FIDO（usage page 0xF1D0）: 64バイトの CTAPHID レポートを interrupt IN/OUT で中継
 *  - OTP キーボード: タッチ時のキー入力を Mac に打つ
 *  - どちらの口でも、フィーチャーレポート（8バイト）= OTP のチャレンジレスポンス・スロット設定を受け付ける。
 *    本物の YubiKey も両方で受け付け、KeePassXC（ykpers）は macOS では FIDO 側を使う
 *    （キーボードを開くには「入力監視」の許可が要るため）。ykman はキーボード側を使う。
 *    Mac の GET_REPORT にはその場で答えず、机上ユニット経由で YubiKey に聞いてから答える
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "tusb.h"
#include "device/usbd_pvt.h"
#include "class/hid/hid.h"

/* YubiKey の OTP インターフェースと同じ形: ブートキーボード + 8バイトのフィーチャーレポート */
#define OTPKBD_REPORT_DESC_LEN 73
#define TUD_OTPKBD_DESC_LEN    (9 + 9 + 7)
#define TUD_OTPKBD_DESCRIPTOR(_itf, _stridx, _epin) \
    9, TUSB_DESC_INTERFACE, _itf, 0, 1, TUSB_CLASS_HID, 1, 1, _stridx, \
    9, HID_DESC_TYPE_HID, U16_TO_U8S_LE(0x0111), 0, 1, HID_DESC_TYPE_REPORT, U16_TO_U8S_LE(OTPKBD_REPORT_DESC_LEN), \
    7, TUSB_DESC_ENDPOINT, _epin, TUSB_XFER_INTERRUPT, U16_TO_U8S_LE(8), 10

/* FIDO: 本物の YubiKey と同じレポートディスクリプタ（入出力 64 バイト） */
#define FIDO_REPORT_LEN        64
#define FIDO_REPORT_DESC_LEN   34
#define TUD_FIDOHID_DESC_LEN   (9 + 9 + 7 + 7)
#define TUD_FIDOHID_DESCRIPTOR(_itf, _stridx, _epout, _epin) \
    9, TUSB_DESC_INTERFACE, _itf, 0, 2, TUSB_CLASS_HID, 0, 0, _stridx, \
    9, HID_DESC_TYPE_HID, U16_TO_U8S_LE(0x0111), 0, 1, HID_DESC_TYPE_REPORT, U16_TO_U8S_LE(FIDO_REPORT_DESC_LEN), \
    7, TUSB_DESC_ENDPOINT, _epout, TUSB_XFER_INTERRUPT, U16_TO_U8S_LE(FIDO_REPORT_LEN), 1, \
    7, TUSB_DESC_ENDPOINT, _epin, TUSB_XFER_INTERRUPT, U16_TO_U8S_LE(FIDO_REPORT_LEN), 1

const usbd_class_driver_t *hid_dev_driver(void);

/* FIDO: Mac から OUT レポートが届いたら呼ばれる（TinyUSB タスク） */
void hid_dev_set_fido_rx(void (*cb)(const uint8_t *report));

/* FIDO: IN レポートを Mac へ。Mac が受け取るまで待つ（最大 timeout_ms）。false = 送れなかった */
bool hid_dev_fido_send(const uint8_t *report, uint32_t timeout_ms);
bool hid_dev_mounted(void);

void hid_dev_init(void);

/* 机上ユニットが「キー入力を中継できる」「フィーチャーレポートを中継できる」か */
void hid_dev_set_otp_caps(bool kbd, bool ctl);

/* YD_CH_OTP / YD_CH_OTPCTL（yd_link 受信タスクから） */
void hid_dev_kbd_from_link(const uint8_t *data, uint16_t len);
void hid_dev_ctl_from_link(const uint8_t *data, uint16_t len);
