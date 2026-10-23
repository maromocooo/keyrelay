/*
 * otp_batch: YubiKey のキー入力（8バイトのレポート）を溜めて、送る分を切り出す
 *
 * 切り出すのは「全部のキーが離された」レポートで終わるところまで。押したキーと、それを離す
 * レポートが別のパケットに分かれないので、レシーバーが押したまま BLE を待つことがない
 * （Mac のキーリピート防止）。押したまま異常に長いときだけ、force で途中まで出す。
 *
 * ESP-IDF に依存しない。PC 上でテストする（firmware/tests/host/test_otp_batch.c）。
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#define OTPB_REPORT_LEN 8
#define OTPB_CAP        256      /* 溜められるレポート数（128 文字分） */

typedef struct {
    uint8_t r[OTPB_CAP][OTPB_REPORT_LEN];
    uint16_t head;               /* 一番古いレポート */
    uint16_t count;
} otpb_ring_t;

static inline void otpb_init(otpb_ring_t *q)
{
    memset(q, 0, sizeof(*q));
}

/* キーが1つも押されていないレポートか（修飾キーだけのものも「離した」とみなす。元の実装と同じ） */
static inline bool otpb_released(const uint8_t *rep)
{
    for (int i = 2; i < OTPB_REPORT_LEN; i++) {
        if (rep[i]) {
            return false;
        }
    }
    return true;
}

/* 1つ足す。一杯なら false（そのレポートは捨てる） */
static inline bool otpb_push(otpb_ring_t *q, const uint8_t rep[OTPB_REPORT_LEN])
{
    if (q->count >= OTPB_CAP) {
        return false;
    }
    memcpy(q->r[(q->head + q->count) % OTPB_CAP], rep, OTPB_REPORT_LEN);
    q->count++;
    return true;
}

/* 最大 max 個を out に取り出す。ふだんは「離した」レポートで終わるところまで（なければ 0 個）。
 * force なら、区切りがなくても max 個まで出す。押したままのレポートが max 個以上続いて区切りが
 * 入らないときも max 個出す（待っても1パケットに収まらないので、止まらないように）。
 * 戻り値は取り出した個数 */
static inline int otpb_take(otpb_ring_t *q, uint8_t *out, int max, bool force)
{
    int n = q->count < max ? q->count : max;
    int cut = 0;
    for (int i = 0; i < n; i++) {
        if (otpb_released(q->r[(q->head + i) % OTPB_CAP])) {
            cut = i + 1;
        }
    }
    if (force || (cut == 0 && q->count >= max)) {
        cut = n;
    }
    for (int i = 0; i < cut; i++) {
        memcpy(out + i * OTPB_REPORT_LEN, q->r[q->head], OTPB_REPORT_LEN);
        q->head = (q->head + 1) % OTPB_CAP;
    }
    q->count -= cut;
    return cut;
}
