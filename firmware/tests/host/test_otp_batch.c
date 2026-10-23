/* Host test for otp_batch.h (desk unit: buffering YubiKey key reports and cutting them into packets).
 * Checks: order and completeness, every packet ends on a release (no key held across a BLE gap), the
 * per-packet limit, the forced cut for a held key, the ring capacity and wrap-around. */
#include <stdio.h>
#include <stdlib.h>
#include "otp_batch.h"

static int s_fail, s_pass;
#define CHECK(cond, msg) do { if (cond) { s_pass++; } else { s_fail++; printf("FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)

static void press(uint8_t *r, uint8_t mod, uint8_t key) { memset(r, 0, 8); r[0] = mod; r[2] = key; }

int main(void)
{
    static otpb_ring_t q;
    static uint8_t in[4096][8], out[4096][8], pkt[24 * 8];

    /* random typing: press (sometimes with Shift), release; taken every few reports at random sizes */
    srand(1);
    int bad_end = 0, too_many = 0, order_ok = 1;
    for (int round = 0; round < 200; round++) {
        otpb_init(&q);
        int nin = 0, nout = 0;
        int chars = 1 + rand() % 120;
        for (int c = 0; c < chars; c++) {
            uint8_t mod = (rand() % 5 == 0) ? 0x02 : 0;
            press(in[nin++], mod, (uint8_t)(4 + rand() % 30));
            if (mod && rand() % 2) {
                press(in[nin++], mod, 0);       /* key up first, Shift still down ("released") */
            }
            press(in[nin++], 0, 0);
        }
        int fed = 0;
        int rpp = 3 + rand() % 22;              /* a character is at most 3 reports here */
        while (nout < nin) {
            int burst = 1 + rand() % 7;            /* reports arriving between two ticks */
            for (int i = 0; i < burst && fed < nin; i++) {
                if (!otpb_push(&q, in[fed])) {
                    printf("unexpected full ring\n");
                    return 1;
                }
                fed++;
            }
            int n = otpb_take(&q, pkt, rpp, false);
            if (n > rpp) {
                too_many++;
            }
            if (n > 0 && !otpb_released(&pkt[(n - 1) * 8])) {
                bad_end++;
            }
            memcpy(out[nout], pkt, (size_t)n * 8);
            nout += n;
            if (fed == nin && n == 0 && q.count) {
                /* everything has arrived but nothing could be cut: only possible for a held key */
                printf("stuck with %u reports\n", q.count);
                return 1;
            }
        }
        if (nout != nin || memcmp(in, out, (size_t)nin * 8) != 0) {
            order_ok = 0;
        }
    }
    CHECK(order_ok, "every report comes out once, in order");
    CHECK(bad_end == 0, "every packet ends on a release");
    CHECK(too_many == 0, "never more than the per-packet limit");

    /* a held key is not cut unless forced */
    otpb_init(&q);
    press(in[0], 0, 0x06);
    otpb_push(&q, in[0]);
    CHECK(otpb_take(&q, pkt, 24, false) == 0 && q.count == 1, "a held key stays in the buffer");
    CHECK(otpb_take(&q, pkt, 24, true) == 1 && q.count == 0 && pkt[2] == 0x06, "forced cut sends the held key");

    /* a release beyond the per-packet limit: cut at the last release inside the limit */
    otpb_init(&q);
    for (int i = 0; i < 30; i++) {
        press(in[i], 0, (i % 2) ? 0 : 0x07);
        otpb_push(&q, in[i]);
    }
    int n = otpb_take(&q, pkt, 5, false);
    CHECK(n == 4 && otpb_released(&pkt[3 * 8]) && q.count == 26, "cut at the last release within the limit");

    /* more held reports than fit in a packet: send a full packet instead of waiting forever */
    otpb_init(&q);
    for (int i = 0; i < 30; i++) {
        press(in[i], 0, (uint8_t)(4 + i));
        otpb_push(&q, in[i]);
    }
    press(in[30], 0, 0);
    otpb_push(&q, in[30]);
    n = otpb_take(&q, pkt, 24, false);
    CHECK(n == 24 && q.count == 7, "a run longer than a packet is cut at the packet size");
    CHECK(otpb_take(&q, pkt, 24, false) == 7 && q.count == 0, "the rest follows up to the release");

    /* capacity and wrap-around */
    otpb_init(&q);
    int pushed = 0;
    for (int i = 0; i < OTPB_CAP + 10; i++) {
        press(in[0], 0, 0);
        pushed += otpb_push(&q, in[0]);
    }
    CHECK(pushed == OTPB_CAP, "ring holds exactly OTPB_CAP reports");
    int total = 0;
    for (int k = 0; k < 2000; k++) {
        press(in[0], 0, 0);
        otpb_take(&q, pkt, 24, false);
        while (q.count < OTPB_CAP) {
            otpb_push(&q, in[0]);
        }
        total += 24;
    }
    CHECK(q.head < OTPB_CAP && q.count == OTPB_CAP, "wrap-around keeps the indices in range");

    printf("otp_batch: %d passed, %d failed\n", s_pass, s_fail);
    return s_fail ? 1 : 0;
}
