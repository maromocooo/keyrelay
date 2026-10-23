/* libFuzzer target: everything the BLE link accepts from the air before it trusts the peer.
 *  - HELLO parsing (before authentication)
 *  - X25519 with an arbitrary peer public key
 *  - AES-GCM open + padding removal of an arbitrary DATA packet
 *  - seal/open round trip of an arbitrary plaintext at every MTU
 * Build and run: make -C firmware/tests/fuzz run */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "yd_sec.h"

static int rng(void *ctx, unsigned char *buf, size_t len)
{
    (void)ctx;
    memset(buf, 0x5A, len);
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static const uint8_t psk[32] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
                                     17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32 };
    static const uint8_t nc[16] = { 0xA0 }, np[16] = { 0xC0 }, epk_c[32] = { 9 }, epk_p[32] = { 9 }, dh[32] = { 7 };
    static yd_sec_keys_t k;
    static int init;
    if (!init) {
        if (yd_sec_derive(psk, nc, np, epk_c, epk_p, dh, &k) != 0) {
            abort();
        }
        init = 1;
    }

    /* HELLO */
    uint8_t n_out[YD_SEC_NONCE_LEN], e_out[YD_SEC_EPK_LEN], ver;
    int hr = yd_sec_hello_parse(data, size, YD_SEC_ROLE_CENTRAL, n_out, e_out, &ver);
    if (hr == YD_SEC_HELLO_OK && size != YD_SEC_HELLO_LEN) {
        abort();
    }

    /* X25519 with an arbitrary public key */
    if (size >= 32) {
        uint8_t sk[32] = { 0x42 }, out[32];
        if (yd_sec_x25519_shared(sk, data, out, rng, NULL) == 0) {
            uint8_t acc = 0;
            for (int i = 0; i < 32; i++) {
                acc |= out[i];
            }
            if (!acc) {
                abort();   /* an all-zero secret must never be accepted */
            }
        }
    }

    /* open an arbitrary packet */
    uint8_t pt[YD_SEC_MAX_PT];
    uint32_t ctr;
    int n = yd_sec_open(k.k_c2p, data, size, &ctr, pt, sizeof(pt));
    if (n >= 0 && (size_t)n + YD_SEC_OVERHEAD + YD_SEC_PAD_MIN > size) {
        abort();
    }

    /* round trip: the input as plaintext, the first byte picks the MTU */
    if (size >= 1) {
        size_t cap = 23 + data[0];               /* 23..278 bytes */
        const uint8_t *m = data + 1;
        size_t mlen = size - 1;
        uint8_t wire[300];
        if (cap > sizeof(wire)) {
            cap = sizeof(wire);
        }
        int s = yd_sec_seal(k.k_p2c, 1, m, mlen, wire, cap);
        size_t expect = yd_sec_sealed_len(mlen, cap);
        if ((s < 0) != (expect == 0) || (s >= 0 && (size_t)s != expect) || (s >= 0 && (size_t)s > cap)) {
            abort();
        }
        if (s > 0) {
            int o = yd_sec_open(k.k_p2c, wire, (size_t)s, &ctr, pt, sizeof(pt));
            if (o != (int)mlen || memcmp(pt, m, mlen) != 0 || ctr != 1) {
                abort();
            }
        }
    }
    return 0;
}
