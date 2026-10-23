/*
 * yd_ccid: USB CCID（スマートカードリーダー）メッセージの最小限の定義
 *
 * CCID メッセージ = 10バイトのヘッダー + データ
 *   [0] bMessageType  [1..4] dwLength(LE, データ部の長さ)  [5] bSlot  [6] bSeq  [7..9] 種類ごと
 *   応答（RDR_to_PC）の [7] = bStatus, [8] = bError
 *
 * KeyRelay は中身（APDU）を解釈せず、メッセージ単位で YubiKey とそのまま中継する。
 * ここにあるのは「YubiKey に届けられないときに自分で返すエラー応答」を作るためのもの。
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define YD_CCID_HDR            10
#define YD_CCID_MAX_MSG        3072        /* dwMaxCCIDMessageLength（YubiKey 5 と同じ。ヘッダー込み） */
#define YD_CCID_BUF            3136        /* 受信バッファ（64の倍数で、最大長より少し大きく） */

/* PC → リーダー */
#define CCID_PC_SET_PARAMS     0x61
#define CCID_PC_POWER_ON       0x62
#define CCID_PC_POWER_OFF      0x63
#define CCID_PC_GET_SLOT       0x65
#define CCID_PC_SECURE         0x69
#define CCID_PC_T0APDU         0x6A
#define CCID_PC_ESCAPE         0x6B
#define CCID_PC_GET_PARAMS     0x6C
#define CCID_PC_RESET_PARAMS   0x6D
#define CCID_PC_ICC_CLOCK      0x6E
#define CCID_PC_XFR_BLOCK      0x6F
#define CCID_PC_MECHANICAL     0x71
#define CCID_PC_ABORT          0x72
#define CCID_PC_SET_RATE       0x73

/* リーダー → PC */
#define CCID_RDR_DATA_BLOCK    0x80
#define CCID_RDR_SLOT_STATUS   0x81
#define CCID_RDR_PARAMS        0x82
#define CCID_RDR_ESCAPE        0x83
#define CCID_RDR_RATE          0x84
#define CCID_RDR_NOTIFY_SLOT   0x50        /* 割り込みエンドポイント */

/* bStatus */
#define CCID_ICC_ACTIVE        0x00
#define CCID_ICC_INACTIVE      0x01
#define CCID_ICC_ABSENT        0x02
#define CCID_CMD_OK            0x00
#define CCID_CMD_FAILED        0x40
#define CCID_CMD_TIME_EXT      0x80

/* bError */
#define CCID_ERR_CMD_NOT_SUPPORTED 0x00
#define CCID_ERR_BAD_SLOT          0x05
#define CCID_ERR_SLOT_BUSY         0xE0
#define CCID_ERR_ICC_MUTE          0xFE

static inline uint32_t yd_ccid_dwlen(const uint8_t *m)
{
    return (uint32_t)m[1] | ((uint32_t)m[2] << 8) | ((uint32_t)m[3] << 16) | ((uint32_t)m[4] << 24);
}

/* 要求の種類に対応する応答の種類 */
static inline uint8_t yd_ccid_resp_type(uint8_t req_type)
{
    switch (req_type) {
    case CCID_PC_POWER_ON:
    case CCID_PC_XFR_BLOCK:
    case CCID_PC_SECURE:
        return CCID_RDR_DATA_BLOCK;
    case CCID_PC_SET_PARAMS:
    case CCID_PC_GET_PARAMS:
    case CCID_PC_RESET_PARAMS:
        return CCID_RDR_PARAMS;
    case CCID_PC_ESCAPE:
        return CCID_RDR_ESCAPE;
    case CCID_PC_SET_RATE:
        return CCID_RDR_RATE;
    default:
        return CCID_RDR_SLOT_STATUS;
    }
}

/* データ部なしの応答を out（10バイト）に作る */
static inline size_t yd_ccid_make_resp(const uint8_t *req, uint8_t status, uint8_t error, uint8_t out[YD_CCID_HDR])
{
    out[0] = yd_ccid_resp_type(req[0]);
    out[1] = out[2] = out[3] = out[4] = 0;
    out[5] = req[5];       /* bSlot */
    out[6] = req[6];       /* bSeq（要求と同じ番号で返す） */
    out[7] = status;
    out[8] = error;
    out[9] = 0;
    return YD_CCID_HDR;
}

/* YubiKey に届かない（未接続・リンク断）ときの応答 */
static inline size_t yd_ccid_make_absent_resp(const uint8_t *req, uint8_t out[YD_CCID_HDR])
{
    switch (req[0]) {
    case CCID_PC_GET_SLOT:
    case CCID_PC_POWER_OFF:
    case CCID_PC_ABORT:
        return yd_ccid_make_resp(req, CCID_CMD_OK | CCID_ICC_ABSENT, 0, out);
    case CCID_PC_POWER_ON:
    case CCID_PC_XFR_BLOCK:
    case CCID_PC_SECURE:
    case CCID_PC_SET_PARAMS:
    case CCID_PC_GET_PARAMS:
    case CCID_PC_RESET_PARAMS:
    case CCID_PC_ESCAPE:
    case CCID_PC_ICC_CLOCK:
    case CCID_PC_T0APDU:
    case CCID_PC_MECHANICAL:
    case CCID_PC_SET_RATE:
        return yd_ccid_make_resp(req, CCID_CMD_FAILED | CCID_ICC_ABSENT, CCID_ERR_ICC_MUTE, out);
    default:
        return yd_ccid_make_resp(req, CCID_CMD_FAILED | CCID_ICC_ABSENT, CCID_ERR_CMD_NOT_SUPPORTED, out);
    }
}

/* PC から届いたメッセージとして形が正しいか（長さがヘッダーと一致するか） */
static inline bool yd_ccid_valid(const uint8_t *m, size_t n)
{
    return n >= YD_CCID_HDR && n <= YD_CCID_MAX_MSG && yd_ccid_dwlen(m) == n - YD_CCID_HDR;
}
