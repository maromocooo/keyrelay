/*
 * yd_frag: 1回の yd_link_send に収まらない長いメッセージ（CCID は最大3KB強）を分割して送る
 *
 *   1片 = [flags(1) | seq(1) | データ]
 *     flags: bit0 = 先頭片, bit1 = 最終片（1片で終わるときは両方）
 *     seq  : 先頭片を 0 として1ずつ増える（取りこぼし検出用）
 *
 * BLE はパケットの順序を保つので、受信側は「先頭片で開始→順番どおりに連結→最終片で完成」するだけ。
 * 途中で番号が飛んだり、長さが上限を超えたら、そのメッセージは捨てて次の先頭片を待つ。
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define YD_FRAG_START      0x01
#define YD_FRAG_END        0x02
#define YD_FRAG_HDR        2
#define YD_FRAG_MAX_CHUNK  200      /* 1片のデータ部の上限（MTU 247 なら 222 まで入るが余裕を見る） */

typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t len;
    uint8_t next_seq;
    bool active;
    uint32_t dropped;               /* 壊れて捨てたメッセージ数（統計） */
} yd_frag_rx_t;

void yd_frag_rx_init(yd_frag_rx_t *r, uint8_t *buf, size_t cap);
void yd_frag_rx_reset(yd_frag_rx_t *r);

/* 1片を与える。戻り値: 完成したメッセージの長さ（r->buf に入っている）/ 0 = 途中 / -1 = 壊れていたので破棄 */
int yd_frag_rx_feed(yd_frag_rx_t *r, const uint8_t *p, uint16_t n);

/* msg を分割して ch に送る。複数タスクから呼んでも片が混ざらない */
esp_err_t yd_frag_send(uint8_t ch, const uint8_t *msg, size_t len);

/* yd_link_init から呼ばれる */
void yd_frag_init(void);

#ifdef __cplusplus
}
#endif
