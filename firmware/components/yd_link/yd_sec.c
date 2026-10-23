#include <string.h>
#include "yd_sec.h"
#include "mbedtls/md.h"
#include "mbedtls/gcm.h"
#include "mbedtls/ecp.h"
#include "mbedtls/ecdh.h"

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

/* ======================================================================
 *  X25519（mbedtls の Curve25519。mbedtls 2.28 と 3.x の両方で動く書き方）
 * ====================================================================== */
static int x25519_load_priv(mbedtls_mpi *d, const uint8_t priv[YD_SEC_EPK_LEN])
{
    uint8_t k[YD_SEC_EPK_LEN];
    memcpy(k, priv, sizeof(k));
    k[0] &= 248;            /* RFC 7748 の clamp */
    k[31] &= 127;
    k[31] |= 64;
    int rc = mbedtls_mpi_read_binary_le(d, k, sizeof(k));
    yd_sec_wipe(k, sizeof(k));
    return rc;
}

int yd_sec_x25519_public(const uint8_t priv[YD_SEC_EPK_LEN], uint8_t pub_out[YD_SEC_EPK_LEN],
                         yd_sec_rng_t rng, void *rng_ctx)
{
    mbedtls_ecp_group grp;
    mbedtls_mpi d;
    mbedtls_ecp_point q;
    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&d);
    mbedtls_ecp_point_init(&q);
    size_t olen = 0;
    int rc = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_CURVE25519);
    if (rc == 0) rc = x25519_load_priv(&d, priv);
    if (rc == 0) rc = mbedtls_ecp_mul(&grp, &q, &d, &grp.G, rng, rng_ctx);
    if (rc == 0) rc = mbedtls_ecp_point_write_binary(&grp, &q, MBEDTLS_ECP_PF_UNCOMPRESSED, &olen,
                                                     pub_out, YD_SEC_EPK_LEN);
    if (rc == 0 && olen != YD_SEC_EPK_LEN) rc = -1;
    mbedtls_ecp_point_free(&q);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_group_free(&grp);
    if (rc != 0) {
        yd_sec_wipe(pub_out, YD_SEC_EPK_LEN);
    }
    return rc;
}

int yd_sec_x25519_shared(const uint8_t priv[YD_SEC_EPK_LEN], const uint8_t peer_pub[YD_SEC_EPK_LEN],
                         uint8_t shared_out[YD_SEC_EPK_LEN], yd_sec_rng_t rng, void *rng_ctx)
{
    mbedtls_ecp_group grp;
    mbedtls_mpi d, z;
    mbedtls_ecp_point q;
    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&d);
    mbedtls_mpi_init(&z);
    mbedtls_ecp_point_init(&q);
    int rc = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_CURVE25519);
    if (rc == 0) rc = x25519_load_priv(&d, priv);
    if (rc == 0) rc = mbedtls_ecp_point_read_binary(&grp, &q, peer_pub, YD_SEC_EPK_LEN);
    if (rc == 0) rc = mbedtls_ecdh_compute_shared(&grp, &z, &q, &d, rng, rng_ctx);
    if (rc == 0) rc = mbedtls_mpi_write_binary_le(&z, shared_out, YD_SEC_EPK_LEN);
    if (rc == 0) {
        uint8_t acc = 0;     /* 小さい位数の点を渡されると共有値が 0 になる（RFC 7748 §6.1） */
        for (int i = 0; i < YD_SEC_EPK_LEN; i++) {
            acc |= shared_out[i];
        }
        if (acc == 0) {
            rc = -1;
        }
    }
    mbedtls_ecp_point_free(&q);
    mbedtls_mpi_free(&z);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_group_free(&grp);
    if (rc != 0) {
        yd_sec_wipe(shared_out, YD_SEC_EPK_LEN);
        return -1;
    }
    return 0;
}

/* ======================================================================
 *  HELLO
 * ====================================================================== */
void yd_sec_hello_build(uint8_t out[YD_SEC_HELLO_LEN], uint8_t role, const uint8_t nonce[YD_SEC_NONCE_LEN],
                        const uint8_t epk[YD_SEC_EPK_LEN])
{
    out[0] = YD_SEC_T_HELLO;
    out[1] = YD_SEC_VERSION;
    out[2] = role;
    memcpy(out + 3, nonce, YD_SEC_NONCE_LEN);
    memcpy(out + 3 + YD_SEC_NONCE_LEN, epk, YD_SEC_EPK_LEN);
}

int yd_sec_hello_parse(const uint8_t *in, size_t len, uint8_t expect_role, uint8_t nonce_out[YD_SEC_NONCE_LEN],
                       uint8_t epk_out[YD_SEC_EPK_LEN], uint8_t *peer_ver)
{
    if (len < 2 || in[0] != YD_SEC_T_HELLO) {
        return YD_SEC_HELLO_MALFORMED;
    }
    if (in[1] != YD_SEC_VERSION) {
        if (peer_ver) {
            *peer_ver = in[1];
        }
        return YD_SEC_HELLO_VERSION;
    }
    if (len != YD_SEC_HELLO_LEN || in[2] != expect_role) {
        return YD_SEC_HELLO_MALFORMED;
    }
    memcpy(nonce_out, in + 3, YD_SEC_NONCE_LEN);
    memcpy(epk_out, in + 3 + YD_SEC_NONCE_LEN, YD_SEC_EPK_LEN);
    return YD_SEC_HELLO_OK;
}

/* ======================================================================
 *  鍵の導出と相互認証
 * ====================================================================== */
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
                  const uint8_t np[YD_SEC_NONCE_LEN], const uint8_t epk_c[YD_SEC_EPK_LEN],
                  const uint8_t epk_p[YD_SEC_EPK_LEN], const uint8_t dh[YD_SEC_EPK_LEN], yd_sec_keys_t *out)
{
    /* salt = nc ‖ np ‖ epk_c ‖ epk_p（やり取りした値をすべて鍵に結び付ける）
     * IKM  = PSK ‖ X25519 の共有値（PSK だけ、共有値だけでは鍵にならない） */
    uint8_t salt[2 * YD_SEC_NONCE_LEN + 2 * YD_SEC_EPK_LEN], ikm[YD_SEC_KEY_LEN + YD_SEC_EPK_LEN], prk[32];
    memcpy(salt, nc, YD_SEC_NONCE_LEN);
    memcpy(salt + YD_SEC_NONCE_LEN, np, YD_SEC_NONCE_LEN);
    memcpy(salt + 2 * YD_SEC_NONCE_LEN, epk_c, YD_SEC_EPK_LEN);
    memcpy(salt + 2 * YD_SEC_NONCE_LEN + YD_SEC_EPK_LEN, epk_p, YD_SEC_EPK_LEN);
    memcpy(ikm, psk, YD_SEC_KEY_LEN);
    memcpy(ikm + YD_SEC_KEY_LEN, dh, YD_SEC_EPK_LEN);
    int rc = hmac256(salt, sizeof(salt), ikm, sizeof(ikm), prk);            /* Extract */
    if (rc == 0) rc = hkdf_expand1(prk, "keyrelay v2 c2p", out->k_c2p);
    if (rc == 0) rc = hkdf_expand1(prk, "keyrelay v2 p2c", out->k_p2c);
    if (rc == 0) rc = hkdf_expand1(prk, "keyrelay v2 auth", out->k_auth);
    yd_sec_wipe(ikm, sizeof(ikm));
    yd_sec_wipe(prk, sizeof(prk));
    if (rc != 0) {
        yd_sec_wipe(out, sizeof(*out));
    }
    return rc;
}

int yd_sec_auth_mac(const yd_sec_keys_t *k, uint8_t role, const uint8_t nc[YD_SEC_NONCE_LEN],
                    const uint8_t np[YD_SEC_NONCE_LEN], uint8_t mac_out[YD_SEC_MAC_LEN])
{
    uint8_t msg[8 + 2 * YD_SEC_NONCE_LEN], full[32] = { 0 };
    memcpy(msg, "YD-AUTH", 7);
    msg[7] = role;
    memcpy(msg + 8, nc, YD_SEC_NONCE_LEN);
    memcpy(msg + 8 + YD_SEC_NONCE_LEN, np, YD_SEC_NONCE_LEN);
    int rc = hmac256(k->k_auth, YD_SEC_KEY_LEN, msg, sizeof(msg), full);
    if (rc == 0) {
        memcpy(mac_out, full, YD_SEC_MAC_LEN);
    } else {
        memset(mac_out, 0, YD_SEC_MAC_LEN);   /* 失敗時に未初期化のスタックを出さない */
    }
    yd_sec_wipe(full, sizeof(full));
    return rc;
}

/* ======================================================================
 *  データ（AES-256-GCM ＋ 埋め草）
 * ====================================================================== */
static void make_iv(uint32_t ctr, uint8_t iv[12])
{
    memset(iv, 0, 12);
    iv[8] = (uint8_t)(ctr >> 24);
    iv[9] = (uint8_t)(ctr >> 16);
    iv[10] = (uint8_t)(ctr >> 8);
    iv[11] = (uint8_t)ctr;
}

/* 埋めた後の平文の長さ。入らないときは 0 */
static size_t padded_len(size_t pt_len, size_t out_cap)
{
    if (out_cap <= YD_SEC_OVERHEAD) {
        return 0;
    }
    size_t lim = out_cap - YD_SEC_OVERHEAD;
    if (lim > YD_SEC_MAX_PT) {
        lim = YD_SEC_MAX_PT;
    }
    size_t need = pt_len + YD_SEC_PAD_MIN;
    if (need > lim) {
        return 0;
    }
    size_t p = (need + YD_SEC_PAD_BLOCK - 1) / YD_SEC_PAD_BLOCK * YD_SEC_PAD_BLOCK;
    return p > lim ? lim : p;
}

size_t yd_sec_sealed_len(size_t pt_len, size_t out_cap)
{
    size_t p = padded_len(pt_len, out_cap);
    return p ? p + YD_SEC_OVERHEAD : 0;
}

int yd_sec_seal(const uint8_t key[YD_SEC_KEY_LEN], uint32_t ctr, const uint8_t *pt, size_t pt_len,
                uint8_t *out, size_t out_cap)
{
    size_t plen = padded_len(pt_len, out_cap);
    if (ctr == 0 || plen == 0) {
        return -1;
    }
    uint8_t buf[YD_SEC_MAX_PT];
    memcpy(buf, pt, pt_len);
    buf[pt_len] = 0x80;                                   /* ISO/IEC 7816-4 の埋め方: 0x80 のあと 0x00 */
    memset(buf + pt_len + 1, 0, plen - pt_len - 1);
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
        rc = mbedtls_gcm_crypt_and_tag(&g, MBEDTLS_GCM_ENCRYPT, plen, iv, sizeof(iv), out, YD_SEC_DATA_HDR,
                                       buf, out + YD_SEC_DATA_HDR, YD_SEC_TAG_LEN, out + YD_SEC_DATA_HDR + plen);
    }
    mbedtls_gcm_free(&g);
    yd_sec_wipe(buf, plen);
    return rc == 0 ? (int)(plen + YD_SEC_OVERHEAD) : -1;
}

int yd_sec_open(const uint8_t key[YD_SEC_KEY_LEN], const uint8_t *in, size_t in_len,
                uint32_t *ctr_out, uint8_t *pt, size_t pt_cap)
{
    if (in_len < YD_SEC_OVERHEAD + YD_SEC_PAD_MIN || in[0] != YD_SEC_T_DATA) {
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
    /* 埋め草を外す（タグの確認が済んでからなので、ここで失敗しても中身の手がかりは出ない） */
    size_t i = n;
    while (i > 0 && pt[i - 1] == 0x00) {
        i--;
    }
    if (i == 0 || pt[i - 1] != 0x80) {
        yd_sec_wipe(pt, n);
        return -1;
    }
    *ctr_out = ctr;
    return (int)(i - 1);
}
