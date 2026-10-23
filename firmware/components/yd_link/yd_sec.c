#include <string.h>
#include "yd_sec.h"
#include "mbedtls/md.h"
#include "mbedtls/gcm.h"

void yd_sec_wipe(void *p, size_t n)
{
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (n--) {
        *v++ = 0;
    }
}

bool yd_sec_equal(const uint8_t *a, const uint8_t *b, size_t n)
{
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) {
        d |= a[i] ^ b[i];
    }
    return d == 0;
}

bool yd_sec_parse_keyfile(const uint8_t *buf, size_t len, uint8_t key_out[YD_SEC_KEY_LEN])
{
    if (len < YD_SEC_KEYFILE_LEN || memcmp(buf, YD_SEC_KEYFILE_MAGIC, 4) != 0) {
        return false;
    }
    const uint8_t *k = buf + 4;
    bool all00 = true, allff = true;
    for (int i = 0; i < YD_SEC_KEY_LEN; i++) {
        all00 &= (k[i] == 0x00);
        allff &= (k[i] == 0xFF);
    }
    if (all00 || allff) {
        return false;
    }
    memcpy(key_out, k, YD_SEC_KEY_LEN);
    return true;
}

static int hmac256(const uint8_t *key, size_t klen, const uint8_t *in, size_t ilen, uint8_t out[32])
{
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    return md ? mbedtls_md_hmac(md, key, klen, in, ilen, out) : -1;
}

/* HKDF-SHA256（RFC 5869）。出力は32バイトなので Expand は T(1) だけ */
static int hkdf_expand1(const uint8_t prk[32], const char *info, uint8_t out[32])
{
    uint8_t buf[64];
    size_t n = strlen(info);
    if (n + 1 > sizeof(buf)) {
        return -1;
    }
    memcpy(buf, info, n);
    buf[n] = 0x01;
    int rc = hmac256(prk, 32, buf, n + 1, out);
    yd_sec_wipe(buf, sizeof(buf));
    return rc;
}

int yd_sec_derive(const uint8_t psk[YD_SEC_KEY_LEN], const uint8_t nc[YD_SEC_NONCE_LEN],
                  const uint8_t np[YD_SEC_NONCE_LEN], yd_sec_keys_t *out)
{
    uint8_t salt[2 * YD_SEC_NONCE_LEN], prk[32];
    memcpy(salt, nc, YD_SEC_NONCE_LEN);
    memcpy(salt + YD_SEC_NONCE_LEN, np, YD_SEC_NONCE_LEN);
    int rc = hmac256(salt, sizeof(salt), psk, YD_SEC_KEY_LEN, prk);            /* Extract */
    if (rc == 0) rc = hkdf_expand1(prk, "keyrelay v1 c2p", out->k_c2p);
    if (rc == 0) rc = hkdf_expand1(prk, "keyrelay v1 p2c", out->k_p2c);
    if (rc == 0) rc = hkdf_expand1(prk, "keyrelay v1 auth", out->k_auth);
    yd_sec_wipe(prk, sizeof(prk));
    if (rc != 0) {
        yd_sec_wipe(out, sizeof(*out));
    }
    return rc;
}

int yd_sec_auth_mac(const yd_sec_keys_t *k, uint8_t role, const uint8_t nc[YD_SEC_NONCE_LEN],
                    const uint8_t np[YD_SEC_NONCE_LEN], uint8_t mac_out[YD_SEC_MAC_LEN])
{
    uint8_t msg[8 + 2 * YD_SEC_NONCE_LEN], full[32];
    memcpy(msg, "YD-AUTH", 7);
    msg[7] = role;
    memcpy(msg + 8, nc, YD_SEC_NONCE_LEN);
    memcpy(msg + 8 + YD_SEC_NONCE_LEN, np, YD_SEC_NONCE_LEN);
    int rc = hmac256(k->k_auth, YD_SEC_KEY_LEN, msg, sizeof(msg), full);
    memcpy(mac_out, full, YD_SEC_MAC_LEN);
    yd_sec_wipe(full, sizeof(full));
    return rc;
}

static void make_iv(uint32_t ctr, uint8_t iv[12])
{
    memset(iv, 0, 12);
    iv[8] = (uint8_t)(ctr >> 24);
    iv[9] = (uint8_t)(ctr >> 16);
    iv[10] = (uint8_t)(ctr >> 8);
    iv[11] = (uint8_t)ctr;
}

int yd_sec_seal(const uint8_t key[YD_SEC_KEY_LEN], uint32_t ctr, const uint8_t *pt, size_t pt_len,
                uint8_t *out, size_t out_cap)
{
    if (ctr == 0 || out_cap < pt_len + YD_SEC_OVERHEAD) {
        return -1;
    }
    out[0] = YD_SEC_T_DATA;
    out[1] = (uint8_t)(ctr >> 24);
    out[2] = (uint8_t)(ctr >> 16);
    out[3] = (uint8_t)(ctr >> 8);
    out[4] = (uint8_t)ctr;
    uint8_t iv[12];
    make_iv(ctr, iv);
    mbedtls_gcm_context g;
    mbedtls_gcm_init(&g);
    int rc = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key, 256);
    if (rc == 0) {
        rc = mbedtls_gcm_crypt_and_tag(&g, MBEDTLS_GCM_ENCRYPT, pt_len, iv, sizeof(iv), out, YD_SEC_DATA_HDR,
                                       pt, out + YD_SEC_DATA_HDR, YD_SEC_TAG_LEN, out + YD_SEC_DATA_HDR + pt_len);
    }
    mbedtls_gcm_free(&g);
    return rc == 0 ? (int)(pt_len + YD_SEC_OVERHEAD) : -1;
}

int yd_sec_open(const uint8_t key[YD_SEC_KEY_LEN], const uint8_t *in, size_t in_len,
                uint32_t *ctr_out, uint8_t *pt, size_t pt_cap)
{
    if (in_len < YD_SEC_OVERHEAD || in[0] != YD_SEC_T_DATA) {
        return -1;
    }
    size_t n = in_len - YD_SEC_OVERHEAD;
    if (n > pt_cap) {
        return -1;
    }
    uint32_t ctr = ((uint32_t)in[1] << 24) | ((uint32_t)in[2] << 16) | ((uint32_t)in[3] << 8) | in[4];
    uint8_t iv[12];
    make_iv(ctr, iv);
    mbedtls_gcm_context g;
    mbedtls_gcm_init(&g);
    int rc = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key, 256);
    if (rc == 0) {
        rc = mbedtls_gcm_auth_decrypt(&g, n, iv, sizeof(iv), in, YD_SEC_DATA_HDR,
                                      in + YD_SEC_DATA_HDR + n, YD_SEC_TAG_LEN, in + YD_SEC_DATA_HDR, pt);
    }
    mbedtls_gcm_free(&g);
    if (rc != 0) {
        yd_sec_wipe(pt, n);
        return -1;
    }
    *ctr_out = ctr;
    return (int)n;
}
