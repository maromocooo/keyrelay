/* Host-side tests for yd_sec (BLE application-layer security, protocol v2).
 *  - known-answer tests against an independent implementation (sec_vectors.h from gen_sec_vectors.py,
 *    Python "cryptography") and the RFC 7748 X25519 test vector
 *  - negative tests: low-order X25519 points, tampered tag / ciphertext / header, wrong key, wrong type,
 *    short input, counter 0, bad padding, buffer limits, HELLO parsing, key-file validation
 * Run: make -C firmware/tests/host */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "yd_sec.h"
#include "sec_vectors.h"

static int s_fail, s_pass;
#define CHECK(cond, msg) do { if (cond) { s_pass++; } else { s_fail++; printf("FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)

/* Blinding RNG for mbedtls. The results do not depend on it. */
static int test_rng(void *ctx, unsigned char *buf, size_t len)
{
    (void)ctx;
    for (size_t i = 0; i < len; i++) {
        buf[i] = (unsigned char)rand();
    }
    return 0;
}

static void hex(const char *s, uint8_t *out, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        sscanf(s + 2 * i, "%2x", &v);
        out[i] = (uint8_t)v;
    }
}

int main(void)
{
    /* ---- X25519: RFC 7748 section 6.1 ---- */
    uint8_t a_sk[32], a_pk[32], b_sk[32], b_pk[32], k_ref[32], out[32], out2[32];
    hex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a", a_sk, 32);
    hex("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a", a_pk, 32);
    hex("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb", b_sk, 32);
    hex("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f", b_pk, 32);
    hex("4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742", k_ref, 32);
    CHECK(yd_sec_x25519_public(a_sk, out, test_rng, NULL) == 0 && memcmp(out, a_pk, 32) == 0, "RFC 7748 Alice public key");
    CHECK(yd_sec_x25519_public(b_sk, out, test_rng, NULL) == 0 && memcmp(out, b_pk, 32) == 0, "RFC 7748 Bob public key");
    CHECK(yd_sec_x25519_shared(a_sk, b_pk, out, test_rng, NULL) == 0 && memcmp(out, k_ref, 32) == 0, "RFC 7748 shared (Alice)");
    CHECK(yd_sec_x25519_shared(b_sk, a_pk, out, test_rng, NULL) == 0 && memcmp(out, k_ref, 32) == 0, "RFC 7748 shared (Bob)");

    /* ---- X25519: vectors used below ---- */
    CHECK(yd_sec_x25519_public(ESK_C, out, test_rng, NULL) == 0 && memcmp(out, EPK_C, 32) == 0, "central public key");
    CHECK(yd_sec_x25519_public(ESK_P, out, test_rng, NULL) == 0 && memcmp(out, EPK_P, 32) == 0, "peripheral public key");
    CHECK(yd_sec_x25519_shared(ESK_C, EPK_P, out, test_rng, NULL) == 0 && memcmp(out, DH, 32) == 0, "shared (central)");
    CHECK(yd_sec_x25519_shared(ESK_P, EPK_C, out2, test_rng, NULL) == 0 && memcmp(out2, DH, 32) == 0, "shared (peripheral)");

    /* ---- X25519: low-order points must be refused and the output wiped ----
     * mbedtls (2.28 and 3.x) already rejects these in its public-key check, before a shared secret exists.
     * The all-zero check in yd_sec_x25519_shared is a second line of defense and is not reached here. */
    static const char *low_order[] = {
        "0000000000000000000000000000000000000000000000000000000000000000",   /* 0 */
        "0100000000000000000000000000000000000000000000000000000000000000",   /* 1 */
        "e0eb7a7c3b41b8ae1656e3faf19fc46ada098deb9c32b1fd866205165f49b800",   /* order 8 */
        "5f9c95bca3508c24b1d0b1559c83ef5b04445cc4581c8e86d8224eddd09f1157",   /* order 8 */
        "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",   /* p - 1 */
    };
    int refused = 0;
    for (size_t i = 0; i < sizeof(low_order) / sizeof(low_order[0]); i++) {
        uint8_t lp[32];
        hex(low_order[i], lp, 32);
        memset(out, 0xAA, sizeof(out));
        int rc = yd_sec_x25519_shared(ESK_C, lp, out, test_rng, NULL);
        uint8_t acc = 0;
        for (int j = 0; j < 32; j++) {
            acc |= out[j];
        }
        if (rc != 0 && acc == 0) {
            refused++;
        }
    }
    CHECK(refused == (int)(sizeof(low_order) / sizeof(low_order[0])), "low-order public keys are refused (by mbedtls), output wiped");

    /* ---- HELLO ---- */
    uint8_t hello[YD_SEC_HELLO_LEN], n_out[16], e_out[32], ver = 0;
    yd_sec_hello_build(hello, YD_SEC_ROLE_CENTRAL, NC, EPK_C);
    CHECK(hello[0] == YD_SEC_T_HELLO && hello[1] == YD_SEC_VERSION && hello[2] == 'C', "HELLO header");
    CHECK(yd_sec_hello_parse(hello, sizeof(hello), YD_SEC_ROLE_CENTRAL, n_out, e_out, &ver) == YD_SEC_HELLO_OK &&
          memcmp(n_out, NC, 16) == 0 && memcmp(e_out, EPK_C, 32) == 0, "HELLO round trip");
    CHECK(yd_sec_hello_parse(hello, sizeof(hello), YD_SEC_ROLE_PERIPHERAL, n_out, e_out, &ver) == YD_SEC_HELLO_MALFORMED,
          "HELLO with the wrong role (reflection)");
    CHECK(yd_sec_hello_parse(hello, sizeof(hello) - 1, YD_SEC_ROLE_CENTRAL, n_out, e_out, &ver) == YD_SEC_HELLO_MALFORMED,
          "short HELLO");
    uint8_t v1[3 + 16] = { YD_SEC_T_HELLO, 1, 'C' };
    CHECK(yd_sec_hello_parse(v1, sizeof(v1), YD_SEC_ROLE_CENTRAL, n_out, e_out, &ver) == YD_SEC_HELLO_VERSION && ver == 1,
          "version-1 HELLO is reported as a version mismatch");
    CHECK(yd_sec_hello_parse(hello, 1, YD_SEC_ROLE_CENTRAL, n_out, e_out, &ver) == YD_SEC_HELLO_MALFORMED, "1-byte HELLO");

    /* ---- key derivation and mutual-auth MAC ---- */
    yd_sec_keys_t k;
    CHECK(yd_sec_derive(PSK, NC, NP, EPK_C, EPK_P, DH, &k) == 0, "derive returns 0");
    CHECK(memcmp(k.k_c2p, K_C2P, 32) == 0, "k_c2p matches reference");
    CHECK(memcmp(k.k_p2c, K_P2C, 32) == 0, "k_p2c matches reference");
    CHECK(memcmp(k.k_auth, K_AUTH, 32) == 0, "k_auth matches reference");

    uint8_t mac[YD_SEC_MAC_LEN];
    CHECK(yd_sec_auth_mac(&k, YD_SEC_ROLE_CENTRAL, NC, NP, mac) == 0 && memcmp(mac, MAC_C, 16) == 0, "central MAC");
    CHECK(yd_sec_auth_mac(&k, YD_SEC_ROLE_PERIPHERAL, NC, NP, mac) == 0 && memcmp(mac, MAC_P, 16) == 0, "peripheral MAC");
    CHECK(memcmp(MAC_C, MAC_P, 16) != 0, "roles give different MACs");

    /* every input must change the keys */
    yd_sec_keys_t k2;
    uint8_t t32[32], t16[16];
    memcpy(t32, PSK, 32); t32[31] ^= 1;
    yd_sec_derive(t32, NC, NP, EPK_C, EPK_P, DH, &k2);
    CHECK(memcmp(k2.k_auth, K_AUTH, 32) != 0, "different PSK gives different keys");
    memcpy(t32, DH, 32); t32[0] ^= 1;
    yd_sec_derive(PSK, NC, NP, EPK_C, EPK_P, t32, &k2);
    CHECK(memcmp(k2.k_auth, K_AUTH, 32) != 0, "different X25519 secret gives different keys (forward secrecy input)");
    memcpy(t32, EPK_P, 32); t32[5] ^= 1;
    yd_sec_derive(PSK, NC, NP, EPK_C, t32, DH, &k2);
    CHECK(memcmp(k2.k_c2p, K_C2P, 32) != 0, "different public key gives different keys (transcript binding)");
    memcpy(t16, NC, 16); t16[0] ^= 1;
    yd_sec_derive(PSK, t16, NP, EPK_C, EPK_P, DH, &k2);
    CHECK(memcmp(k2.k_c2p, K_C2P, 32) != 0, "different nonce gives different keys");

    /* ---- seal: known answer, padding ---- */
    uint8_t buf[300], pt[300];
    int n = yd_sec_seal(k.k_c2p, CTR, PT, sizeof(PT), buf, sizeof(buf));
    CHECK(n == (int)sizeof(SEALED) && memcmp(buf, SEALED, sizeof(SEALED)) == 0, "seal matches reference (padded to 32)");
    n = yd_sec_seal(k.k_c2p, CTR + 1, PT31, sizeof(PT31), buf, sizeof(buf));
    CHECK(n == (int)sizeof(SEALED31) && memcmp(buf, SEALED31, sizeof(SEALED31)) == 0, "31-byte plaintext fills one block");
    n = yd_sec_seal(k.k_c2p, CTR + 2, PT_ZEROS, sizeof(PT_ZEROS), buf, sizeof(buf));
    CHECK(n == (int)sizeof(SEALED_ZEROS) && memcmp(buf, SEALED_ZEROS, sizeof(SEALED_ZEROS)) == 0, "trailing zeros reference");
    CHECK(yd_sec_seal(k.k_c2p, 0, PT, sizeof(PT), buf, sizeof(buf)) < 0, "counter 0 is rejected");
    CHECK(yd_sec_seal(k.k_c2p, CTR, PT, sizeof(PT), buf, sizeof(PT) + YD_SEC_OVERHEAD) < 0, "no room for the padding marker");
    CHECK(yd_sec_seal(k.k_c2p, CTR, PT, sizeof(PT), buf, sizeof(PT) + 1 + YD_SEC_OVERHEAD) == (int)(sizeof(PT) + 1 + YD_SEC_OVERHEAD),
          "padding is capped by the output buffer");
    CHECK(yd_sec_sealed_len(sizeof(PT), sizeof(buf)) == sizeof(SEALED), "sealed_len matches seal");
    CHECK(yd_sec_sealed_len(200, 244) == 223 + YD_SEC_OVERHEAD, "largest BLE packet: capped at the MTU");
    CHECK(yd_sec_sealed_len(64 + 1, 244) == 96 + YD_SEC_OVERHEAD, "FIDO report pads to 96");
    /* OpenPGP VERIFY with an n-digit PIN: channel 1 + fragment header 2 + CCID header 10 + APDU header 5 + n.
     * PINs of 1..13 characters give the same size on the air */
    size_t same = yd_sec_sealed_len(18 + 1, 244);
    int all_same = 1;
    for (size_t pin = 1; pin <= 13; pin++) {
        all_same &= yd_sec_sealed_len(18 + pin, 244) == same;
    }
    CHECK(all_same, "PIN lengths 1..13 are indistinguishable on the air");

    /* ---- open: round trips ---- */
    uint32_t ctr = 0;
    n = yd_sec_open(k.k_c2p, SEALED, sizeof(SEALED), &ctr, pt, sizeof(pt));
    CHECK(n == (int)sizeof(PT) && memcmp(pt, PT, sizeof(PT)) == 0 && ctr == CTR, "open reference packet, padding removed");
    n = yd_sec_open(k.k_c2p, SEALED31, sizeof(SEALED31), &ctr, pt, sizeof(pt));
    CHECK(n == (int)sizeof(PT31) && memcmp(pt, PT31, sizeof(PT31)) == 0 && ctr == CTR + 1, "open one-block packet");
    n = yd_sec_open(k.k_c2p, SEALED_ZEROS, sizeof(SEALED_ZEROS), &ctr, pt, sizeof(pt));
    CHECK(n == (int)sizeof(PT_ZEROS) && memcmp(pt, PT_ZEROS, sizeof(PT_ZEROS)) == 0, "payload ending in zeros survives");
    CHECK(yd_sec_open(k.k_c2p, SEALED, sizeof(SEALED), &ctr, pt, sizeof(SEALED) - YD_SEC_OVERHEAD - 1) < 0,
          "too small plaintext buffer");
    CHECK(yd_sec_open(k.k_c2p, BAD_NOMARK, sizeof(BAD_NOMARK), &ctr, pt, sizeof(pt)) < 0, "authentic but no padding marker");
    CHECK(yd_sec_open(k.k_c2p, BAD_WRONGMARK, sizeof(BAD_WRONGMARK), &ctr, pt, sizeof(pt)) < 0, "authentic but wrong marker");

    /* round trip of every length the link can carry */
    int rt_ok = 1;
    for (size_t len = 1; len <= 222; len++) {
        uint8_t m[222];
        for (size_t i = 0; i < len; i++) {
            m[i] = (uint8_t)(i * 37 + len);
        }
        m[len - 1] = (len % 3) ? 0x00 : 0x80;     /* also end in 0x00 / 0x80 */
        int s = yd_sec_seal(k.k_p2c, (uint32_t)len, m, len, buf, 244);
        int o = s > 0 ? yd_sec_open(k.k_p2c, buf, (size_t)s, &ctr, pt, sizeof(pt)) : -1;
        rt_ok &= (s > 0 && ((s - YD_SEC_OVERHEAD) % YD_SEC_PAD_BLOCK == 0 || s == 244)) && o == (int)len &&
                 memcmp(pt, m, len) == 0 && ctr == len;
    }
    CHECK(rt_ok, "round trip of every length 1..222, padded sizes on 32-byte steps");

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
    CHECK(yd_sec_open(k.k_c2p, SEALED, YD_SEC_OVERHEAD, &ctr, pt, sizeof(pt)) < 0, "no ciphertext at all");

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
