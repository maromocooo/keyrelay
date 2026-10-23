/*
 * yd_sec: BLE リンクの上に重ねるアプリ層の暗号化（事前共有鍵）
 *
 * BLE の Just Works ペアリングは「ペアリングの瞬間」に中間者攻撃を防げない。
 * そこで、2台に同じ 256bit の鍵（keyrelay-key.bin）を書き込んでおき、その鍵を知らない相手とは
 * 一切データをやり取りしないようにする。BLE の暗号化はそのまま残すので二重の暗号化になる。
 *
 *  1. リンクが張れたら、両方が 16バイトの乱数を送り合う（HELLO）
 *  2. 鍵と2つの乱数から、この接続だけで使う鍵を HKDF-SHA256 で作る
 *       k_c2p（レシーバー→机上）, k_p2c（机上→レシーバー）, k_auth（相互確認用）
 *  3. 互いに HMAC(k_auth, "YD-AUTH" || 役割 || 乱数) を送り、相手が同じ鍵を持っていることを確かめる（AUTH）
 *  4. 以後のデータは AES-256-GCM。ヘッダー（種別＋通し番号）も改ざん検知の対象。
 *     通し番号は増える一方でないと受け付けない（使い回し＝リプレイ対策）
 *
 * このファイルは ESP-IDF に依存しない（mbedtls だけ）。PC 上でテストできる。
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define YD_SEC_KEY_LEN     32
#define YD_SEC_NONCE_LEN   16
#define YD_SEC_MAC_LEN     16
#define YD_SEC_TAG_LEN     16
#define YD_SEC_DATA_HDR    5                                  /* type(1) + ctr(4, BE) */
#define YD_SEC_OVERHEAD    (YD_SEC_DATA_HDR + YD_SEC_TAG_LEN)  /* 平文に足されるバイト数 */

/* パケット先頭1バイト目（BLE の1パケット＝1メッセージ） */
#define YD_SEC_T_HELLO     0xA1   /* [T, ver, role, nonce(16)] */
#define YD_SEC_T_AUTH      0xA2   /* [T, mac(16)] */
#define YD_SEC_T_DATA      0xE1   /* [T, ctr(4), 暗号文(ch|payload), tag(16)] */
#define YD_SEC_VERSION     1
#define YD_SEC_ROLE_CENTRAL     'C'   /* レシーバー */
#define YD_SEC_ROLE_PERIPHERAL  'P'   /* 机上ユニット */

/* 鍵ファイル（keyrelay-key.bin）の形式: "YDK1" + 鍵32バイト */
#define YD_SEC_KEYFILE_MAGIC "YDK1"
#define YD_SEC_KEYFILE_LEN   (4 + YD_SEC_KEY_LEN)

typedef struct {
    uint8_t k_c2p[YD_SEC_KEY_LEN];
    uint8_t k_p2c[YD_SEC_KEY_LEN];
    uint8_t k_auth[YD_SEC_KEY_LEN];
} yd_sec_keys_t;

/* 鍵ファイルの中身を検証して鍵を取り出す（全部 0x00 / 0xFF などは不可） */
bool yd_sec_parse_keyfile(const uint8_t *buf, size_t len, uint8_t key_out[YD_SEC_KEY_LEN]);

/* nc = レシーバー（Central）の乱数, np = 机上ユニット（Peripheral）の乱数 */
int yd_sec_derive(const uint8_t psk[YD_SEC_KEY_LEN], const uint8_t nc[YD_SEC_NONCE_LEN],
                  const uint8_t np[YD_SEC_NONCE_LEN], yd_sec_keys_t *out);

/* role が自分の確認コードを作る */
int yd_sec_auth_mac(const yd_sec_keys_t *k, uint8_t role, const uint8_t nc[YD_SEC_NONCE_LEN],
                    const uint8_t np[YD_SEC_NONCE_LEN], uint8_t mac_out[YD_SEC_MAC_LEN]);

/* 定数時間の比較（一致で true） */
bool yd_sec_equal(const uint8_t *a, const uint8_t *b, size_t n);

/* 暗号化: out に [T, ctr, 暗号文, tag] を書き、全体の長さを返す（失敗で -1） */
int yd_sec_seal(const uint8_t key[YD_SEC_KEY_LEN], uint32_t ctr, const uint8_t *pt, size_t pt_len,
                uint8_t *out, size_t out_cap);

/* 復号: 成功で平文の長さ、改ざん・形式不正で -1。ctr_out に通し番号 */
int yd_sec_open(const uint8_t key[YD_SEC_KEY_LEN], const uint8_t *in, size_t in_len,
                uint32_t *ctr_out, uint8_t *pt, size_t pt_cap);

/* 機密データの消去 */
void yd_sec_wipe(void *p, size_t n);

#ifdef __cplusplus
}
#endif
