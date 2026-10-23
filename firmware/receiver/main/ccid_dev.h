/*
 * ccid_dev: レシーバーを Mac から「スマートカードリーダー（USB CCID クラス）」に見せる
 *
 * TinyUSB には CCID クラスがないので、アプリ側クラスドライバー（usbd_app_driver_get_cb）として実装する。
 * Mac から届いた CCID メッセージを丸ごと机上ユニットへ送り、YubiKey の応答をそのまま返すだけ。
 * 机上ユニットまで届かないとき（YubiKey 未接続・BLE 断）は「カードなし」として自分で答える。
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "tusb.h"
#include "device/usbd_pvt.h"

/* ---- ディスクリプタ（構成ディスクリプタに埋め込む） ---- */
#define TUD_CCID_DESC_LEN  (9 + 54 + 7 * 3)
#define CCID_EP_MPS        64

/* YubiKey 5 の CCID 機能ディスクリプタに合わせる:
 *  T=1、自動設定（ATR/電圧/クロック/ボーレート/PPS）、短い/拡張APDU単位の交換、
 *  最大メッセージ 3072 バイト、同時に処理中にできるスロットは1つ */
#define TUD_CCID_DESCRIPTOR(_itf, _stridx, _epout, _epin, _epint) \
    9, TUSB_DESC_INTERFACE, _itf, 0, 3, TUSB_CLASS_SMART_CARD, 0, 0, _stridx, \
    54, 0x21, U16_TO_U8S_LE(0x0110), 0x00, 0x07, \
    U32_TO_U8S_LE(0x00000002), U32_TO_U8S_LE(4000), U32_TO_U8S_LE(4000), 0x00, \
    U32_TO_U8S_LE(10752), U32_TO_U8S_LE(10752), 0x00, U32_TO_U8S_LE(254), \
    U32_TO_U8S_LE(0), U32_TO_U8S_LE(0), U32_TO_U8S_LE(0x000400FE), U32_TO_U8S_LE(3072), \
    0xFF, 0xFF, U16_TO_U8S_LE(0), 0x00, 0x01, \
    7, TUSB_DESC_ENDPOINT, _epout, TUSB_XFER_BULK, U16_TO_U8S_LE(CCID_EP_MPS), 0, \
    7, TUSB_DESC_ENDPOINT, _epin, TUSB_XFER_BULK, U16_TO_U8S_LE(CCID_EP_MPS), 0, \
    7, TUSB_DESC_ENDPOINT, _epint, TUSB_XFER_INTERRUPT, U16_TO_U8S_LE(8), 16

const usbd_class_driver_t *ccid_dev_driver(void);

/* app_main から（TinyUSB を起動する前に） */
void ccid_dev_init(void);

/* カード（= 机上ユニットの YubiKey の CCID）が使えるか。リンク断・取り外しで false */
void ccid_dev_set_card(bool available);

/* 机上ユニットから届いた YD_CH_CCID の1片（yd_link 受信タスクから） */
void ccid_dev_from_link(const uint8_t *data, uint16_t len);
