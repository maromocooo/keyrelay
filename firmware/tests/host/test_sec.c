/* Host-side tests for yd_sec (BLE application-layer security).
 *  - known-answer tests against an independent implementation (sec_vectors.h, Python "cryptography")
 *  - negative tests: tampered tag / ciphertext / header, wrong key, wrong type, short input,
 *    counter 0, buffer limits, key-file validation
 * Run: make -C firmware/tests/host */
#include <stdio.h>
#include <string.h>
#include "yd_sec.h"
#include "sec_vectors.h"

static int s_fail, s_pass;
#define CHECK(cond, msg) do { if (cond) { s_pass++; } else { s_fail++; printf("FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)

int main(void)
{
    /* ---- key derivation and mutual-auth MAC ---- */
    yd_sec_keys_t k;
    CHECK(yd_sec_derive(PSK, NC, NP, &k) == 0, "derive returns 0");
    CHECK(memcmp(k.k_c2p, K_C2P, 32) == 0, "k_c2p matches reference");
    CHECK(memcmp(k.k_p2c, K_P2C, 32) == 0, "k_p2c matches reference");
    CHECK(memcmp(k.k_auth, K_AUTH, 32) == 0, "k_auth matches reference");

    uint8_t mac[YD_SEC_MAC_LEN];
    CHECK(yd_sec_auth_mac(&k, YD_SEC_ROLE_CENTRAL, NC, NP, mac) == 0 && memcmp(mac, MAC_C, 16) == 0, "central MAC");
    CHECK(yd_sec_auth_mac(&k, YD_SEC_ROLE_PERIPHERAL, NC, NP, mac) == 0 && memcmp(mac, MAC_P, 16) == 0, "peripheral MAC");
    CHECK(memcmp(MAC_C, MAC_P, 16) != 0, "roles give different MACs");

    yd_sec_keys_t k2;
    uint8_t psk2[32];
    memcpy(psk2, PSK, 32);
    psk2[31] ^= 1;
    yd_sec_derive(psk2, NC, NP, &k2);
    CHECK(memcmp(k2.k_auth, K_AUTH, 32) != 0, "different PSK gives different keys");
    uint8_t nc2[16];
    memcpy(nc2, NC, 16);
    nc2[0] ^= 1;
    yd_sec_derive(PSK, nc2, NP, &k2);
    CHECK(memcmp(k2.k_c2p, K_C2P, 32) != 0, "different nonce gives different keys");

    /* ---- seal: known answer ---- */
    uint8_t out[64], pt[64];
    int n = yd_sec_seal(k.k_c2p, CTR, PT, sizeof(PT), out, sizeof(out));
    CHECK(n == (int)sizeof(SEALED) && memcmp(out, SEALED, sizeof(SEALED)) == 0, "seal matches reference");
    CHECK(yd_sec_seal(k.k_c2p, 0, PT, sizeof(PT), out, sizeof(out)) < 0, "counter 0 is rejected");
    CHECK(yd_sec_seal(k.k_c2p, CTR, PT, sizeof(PT), out, sizeof(PT) + YD_SEC_OVERHEAD - 1) < 0, "too small output buffer");

    /* ---- open: round trip ---- */
    uint32_t ctr = 0;
    n = yd_sec_open(k.k_c2p, SEALED, sizeof(SEALED), &ctr, pt, sizeof(pt));
    CHECK(n == (int)sizeof(PT) && memcmp(pt, PT, sizeof(PT)) == 0 && ctr == CTR, "open reference packet");
    CHECK(yd_sec_open(k.k_c2p, SEALED, sizeof(SEALED), &ctr, pt, sizeof(PT) - 1) < 0, "too small plaintext buffer");

    /* ---- open: every single-bit change anywhere in the packet must be rejected ---- */
    uint8_t bad[sizeof(SEALED)];
    int accepted = 0;
    for (size_t i = 0; i < sizeof(SEALED); i++) {
        for (int b = 0; b < 8; b++) {
            memcpy(bad, SEALED, sizeof(SEALED));
            bad[i] ^= (uint8_t)(1u << b);
            if (yd_sec_open(k.k_c2p, bad, sizeof(bad), &ctr, pt, sizeof(pt)) >= 0) {
                accepted++;
            }
        }
    }
    CHECK(accepted == 0, "no bit flip (type, counter, ciphertext, tag) is accepted");

    CHECK(yd_sec_open(k.k_p2c, SEALED, sizeof(SEALED), &ctr, pt, sizeof(pt)) < 0, "wrong direction key is rejected");
    CHECK(yd_sec_open(k.k_c2p, SEALED, sizeof(SEALED) - 1, &ctr, pt, sizeof(pt)) < 0, "truncated packet is rejected");
    CHECK(yd_sec_open(k.k_c2p, SEALED, YD_SEC_OVERHEAD - 1, &ctr, pt, sizeof(pt)) < 0, "shorter than overhead");

    /* ---- key file ---- */
    uint8_t kf[YD_SEC_KEYFILE_LEN], key[32];
    memcpy(kf, "YDK1", 4);
    memcpy(kf + 4, PSK, 32);
    CHECK(yd_sec_parse_keyfile(kf, sizeof(kf), key) && memcmp(key, PSK, 32) == 0, "valid key file");
    CHECK(!yd_sec_parse_keyfile(kf, sizeof(kf) - 1, key), "short key file");
    kf[0] = 'X';
    CHECK(!yd_sec_parse_keyfile(kf, sizeof(kf), key), "bad magic");
    memcpy(kf, "YDK1", 4);
    memset(kf + 4, 0x00, 32);
    CHECK(!yd_sec_parse_keyfile(kf, sizeof(kf), key), "all-zero key (erased / unset)");
    memset(kf + 4, 0xFF, 32);
    CHECK(!yd_sec_parse_keyfile(kf, sizeof(kf), key), "all-0xFF key (erased flash)");

    /* ---- constant-time compare ---- */
    CHECK(yd_sec_equal(MAC_C, MAC_C, 16) && !yd_sec_equal(MAC_C, MAC_P, 16), "yd_sec_equal");

    printf("yd_sec: %d passed, %d failed\n", s_pass, s_fail);
    return s_fail ? 1 : 0;
}
