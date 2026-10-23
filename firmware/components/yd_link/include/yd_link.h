/*
 * yd_link: 机上ユニット <-> レシーバー 間の双方向リンク
 *
 * 「チャンネル番号 + ペイロード」の単位でやり取りする。トランスポートは2種類:
 *
 *  - YD_LINK_UART          : 有線UART（MVP初版）。フレーム化 + CRC16
 *      0xA5 0x5A | ch(1) | len(2, LE) | payload(len) | crc16(2, LE)
 *
 *  - YD_LINK_BLE_PERIPHERAL: 机上ユニット側（電池駆動、アドバタイズして待つ）
 *  - YD_LINK_BLE_CENTRAL   : レシーバー側（USB給電、スキャンして接続しに行く）
 *      独自GATTサービス。1回の Write / Notify = [ch(1) | payload]。
 *      LE Secure Connections でボンディングし、暗号化されたリンクでしか中継しない。
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* チャンネル（= YubiKey のUSBインターフェース単位で多重化） */
#define YD_CH_FIDO   0x01   /* CTAPHID 64バイトレポート（FIDO2/U2F） */
#define YD_CH_OTP    0x02   /* OTP キーボードの入力レポート（8バイト×N、押す〜離すのまとまり） */
#define YD_CH_OTPCTL 0x04   /* OTP の HID フィーチャーレポート（チャレンジレスポンス・スロット設定）。下の YD_OTPCTL_* */
#define YD_CH_CCID   0x03   /* CCID メッセージ（PIV/OpenPGP/OATH）。yd_frag で分割して送る */
#define YD_CH_CTRL   0x7F   /* 制御メッセージ */

/* YD_CH_CTRL のペイロード1バイト目 */
#define YD_CTRL_HELLO        0x01   /* 起動 / リンク確立の通知 */
#define YD_CTRL_KEY_ATTACHED 0x02   /* YubiKey 認識完了。2バイト目 = 使える機能（YD_KEY_CAP_*、v5〜） */
#define YD_CTRL_KEY_DETACHED 0x03   /* YubiKey 取り外し */

/* YD_CTRL_KEY_ATTACHED の2バイト目。1バイトだけ（v4以前）のときは FIDO のみとみなす */
#define YD_KEY_CAP_FIDO      0x01
#define YD_KEY_CAP_CCID      0x02
#define YD_KEY_CAP_OTP       0x04
#define YD_KEY_CAP_OTPCTL    0x08   /* フィーチャーレポートの中継に対応（v7〜） */

/* YD_CH_OTPCTL: Mac が出した GET_REPORT / SET_REPORT（Feature, 8バイト）を1回ずつ YubiKey まで往復させる
 *   レシーバー → 机上: [SET, data(8)]            / [GET, tag]
 *   机上 → レシーバー: [GET_RESP, tag, ok, data(8)]
 * BLE は順序を保つので、SET のあとの GET は必ず SET を反映した YubiKey の状態を返す */
#define YD_OTPCTL_SET        0x01
#define YD_OTPCTL_GET        0x02
#define YD_OTPCTL_GET_RESP   0x82
#define YD_OTPCTL_REPORT_LEN 8

#define YD_LINK_MAX_PAYLOAD  1024   /* UART時。BLE時は (ATT MTU - 4) まで */

typedef enum {
    YD_LINK_UART = 0,
    YD_LINK_BLE_PERIPHERAL,
    YD_LINK_BLE_CENTRAL,
} yd_link_transport_t;

typedef void (*yd_link_rx_cb_t)(uint8_t ch, const uint8_t *data, uint16_t len, void *arg);
/* BLE: 暗号化済みで送受信できる状態になった(up=true) / 切れた(up=false) */
typedef void (*yd_link_state_cb_t)(bool up, void *arg);

typedef struct {
    yd_link_transport_t transport;
    /* UART のみ */
    int uart_num;
    int tx_gpio;
    int rx_gpio;
    int baudrate;
    /* 共通 */
    yd_link_rx_cb_t rx_cb;          /* リンク専用タスクのコンテキストで呼ばれる */
    void *rx_cb_arg;
    yd_link_state_cb_t state_cb;    /* 任意 */
    void *state_cb_arg;
    /* BLE Peripheral のみ: 無通信時に接続パラメータを省電力側（長い間隔＋ペリフェラルレイテンシ）
     * へ切り替え、通信があれば即座に低遅延側へ戻す（机上ユニット用。レシーバーは false） */
    bool ble_conn_power_save;
} yd_link_config_t;

esp_err_t yd_link_init(const yd_link_config_t *cfg);

/* スレッドセーフ。BLEでは送信キューに積んで即座に返る */
esp_err_t yd_link_send(uint8_t ch, const uint8_t *data, uint16_t len);

/* 便利関数: 1バイトの制御メッセージ */
static inline esp_err_t yd_link_send_ctrl(uint8_t code)
{
    return yd_link_send(YD_CH_CTRL, &code, 1);
}

/* 1回の yd_link_send で送れる最大バイト数（BLEは MTU と暗号化のオーバーヘッドで決まる。未接続は0） */
uint16_t yd_link_max_payload(void);

/* 送受信できる状態か（UARTは常にtrue） */
bool yd_link_is_up(void);

/* BLE: 保存済みのペアリング情報をすべて消す（相手を変えたいとき） */
esp_err_t yd_link_forget_peers(void);

/* 統計（デバッグ用） */
typedef struct {
    uint32_t rx_frames;
    uint32_t rx_crc_errors;
    uint32_t rx_overflows;
    uint32_t tx_frames;
    uint32_t tx_dropped;
} yd_link_stats_t;

void yd_link_get_stats(yd_link_stats_t *out);

/* 現在の接続パラメータ（状態ログ用）。UART時は connected=true で他は0 */
typedef struct {
    bool connected;
    uint16_t itvl;          /* 接続間隔（1.25ms単位） */
    uint16_t latency;       /* ペリフェラルレイテンシ（スキップできる接続イベント数） */
    uint16_t timeout;       /* スーパービジョンタイムアウト（10ms単位） */
    bool power_save;        /* 省電力パラメータを適用中か */
    bool update_pending;    /* パラメータ更新の手続き中か */
} yd_link_conn_info_t;

void yd_link_get_conn_info(yd_link_conn_info_t *out);

#ifdef __cplusplus
}
#endif
