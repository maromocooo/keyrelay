/*
 * yd_sec: BLE リンクの上に重ねるアプリ層の暗号化（事前共有鍵 ＋ 使い捨ての X25519）
 *
 * BLE の Just Works ペアリングは「ペアリングの瞬間」に中間者攻撃を防げない。
 * そこで、2台に同じ 256bit の鍵（keyrelay-key.bin）を書き込んでおき、その鍵を知らない相手とは
 * 一切データをやり取りしないようにする。BLE の暗号化はそのまま残すので二重の暗号化になる。
 *
 *  1. リンクが張れたら、両方が 16バイトの乱数と、接続ごとに作り直す X25519 の公開鍵を送り合う（HELLO）
 *  2. 事前共有鍵・X25519 の共有値・乱数・公開鍵から、この接続だけで使う鍵を HKDF-SHA256 で作る
 *       k_c2p（レシーバー→机上）, k_p2c（机上→レシーバー）, k_auth（相互確認用）
 *     X25519 の秘密鍵は RAM にしか置かず、鍵を作ったらすぐ消す。あとで事前共有鍵や BLE のボンド
 *     （どちらもフラッシュにある）を読み出されても、録られていた通信は復号できない（前方秘匿性）。
 *  3. 互いに HMAC(k_auth, "YD-AUTH" || 役割 || 乱数) を送り、相手が同じ鍵を持っていることを確かめる（AUTH）
 *  4. 以後のデータは AES-256-GCM。ヘッダー（種別＋通し番号）も改ざん検知の対象。
 *     通し番号は 1 ずつ増えないと受け付けない（リプレイ・並べ替え・抜き取りの検出）。
 *     平文は 32バイト単位に埋めてから暗号化し、PIN などの長さが電波から分からないようにする。
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
#define YD_SEC_EPK_LEN     32                                 /* X25519 の公開鍵・秘密鍵・共有値 */
#define YD_SEC_MAC_LEN     16
#define YD_SEC_TAG_LEN     16
#define YD_SEC_DATA_HDR    5                                  /* type(1) + ctr(4, BE) */
#define YD_SEC_OVERHEAD    (YD_SEC_DATA_HDR + YD_SEC_TAG_LEN)  /* 暗号化で足されるバイト数（埋め草は別） */
#define YD_SEC_PAD_BLOCK   32                                 /* 平文はこの単位に埋める */
#define YD_SEC_PAD_MIN     1                                  /* 埋め草の最小（区切りの 0x80） */
#define YD_SEC_MAX_PT      256                                /* 埋めた後の平文の上限 */

/* パケット先頭1バイト目（BLE の1パケット＝1メッセージ） */
#define YD_SEC_T_HELLO     0xA1   /* [T, ver, role, nonce(16), X25519 公開鍵(32)] */
#define YD_SEC_T_AUTH      0xA2   /* [T, mac(16)] */
#define YD_SEC_T_DATA      0xE1   /* [T, ctr(4), 暗号文(ch|payload|0x80|0x00…), tag(16)] */
#define YD_SEC_VERSION     2      /* 1 = 事前共有鍵だけ（X25519 なし・埋め草なし）。2台は同じ版で動かす */
#define YD_SEC_HELLO_LEN   (3 + YD_SEC_NONCE_LEN + YD_SEC_EPK_LEN)
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

/* mbedtls の楕円曲線演算のブラインディングに使う乱数（結果には影響しない） */
typedef int (*yd_sec_rng_t)(void *ctx, unsigned char *buf, size_t len);

/* 鍵ファイルの中身を検証して鍵を取り出す（全部 0x00 / 0xFF などは不可） */
bool yd_sec_parse_keyfile(const uint8_t *buf, size_t len, uint8_t key_out[YD_SEC_KEY_LEN]);

/* X25519（RFC 7748）。priv は乱数32バイト（clamp はこの中で行う） */
int yd_sec_x25519_public(const uint8_t priv[YD_SEC_EPK_LEN], uint8_t pub_out[YD_SEC_EPK_LEN],
                         yd_sec_rng_t rng, void *rng_ctx);
/* 共有値を作る。相手の公開鍵が小さい位数の点などで共有値が 0 になるときは失敗（-1） */
int yd_sec_x25519_shared(const uint8_t priv[YD_SEC_EPK_LEN], const uint8_t peer_pub[YD_SEC_EPK_LEN],
                         uint8_t shared_out[YD_SEC_EPK_LEN], yd_sec_rng_t rng, void *rng_ctx);

/* HELLO を組み立てる / 読む */
void yd_sec_hello_build(uint8_t out[YD_SEC_HELLO_LEN], uint8_t role, const uint8_t nonce[YD_SEC_NONCE_LEN],
                        const uint8_t epk[YD_SEC_EPK_LEN]);
#define YD_SEC_HELLO_OK        0
#define YD_SEC_HELLO_MALFORMED (-1)
#define YD_SEC_HELLO_VERSION   (-2)   /* 相手が別の版（*peer_ver に版） */
int yd_sec_hello_parse(const uint8_t *in, size_t len, uint8_t expect_role, uint8_t nonce_out[YD_SEC_NONCE_LEN],
                       uint8_t epk_out[YD_SEC_EPK_LEN], uint8_t *peer_ver);

/* nc / epk_c = レシーバー（Central）の乱数と公開鍵, np / epk_p = 机上ユニット（Peripheral）のもの。
 * dh = X25519 の共有値 */
int yd_sec_derive(const uint8_t psk[YD_SEC_KEY_LEN], const uint8_t nc[YD_SEC_NONCE_LEN],
                  const uint8_t np[YD_SEC_NONCE_LEN], const uint8_t epk_c[YD_SEC_EPK_LEN],
                  const uint8_t epk_p[YD_SEC_EPK_LEN], const uint8_t dh[YD_SEC_EPK_LEN], yd_sec_keys_t *out);

/* BLE で互いを見つけるための GATT の UUID（16バイト）。kind = YD_SEC_GATT_*。
 * HMAC-SHA256(PSK, "keyrelay v2 gatt uuid" || kind) の先頭16バイト。同じ PSK の機器だけが同じ値になり、
 * ほかの人には KeyRelay だと分からない（3つの UUID の間にも決まった関係はない）。
 * サービスの UUID の先頭4バイトを「Link ID」としてログに出す */
#define YD_SEC_GATT_UUID_LEN 16
#define YD_SEC_GATT_SVC      0     /* サービス */
#define YD_SEC_GATT_TO_DESK  1     /* レシーバー → 机上（Write Without Response） */
#define YD_SEC_GATT_TO_RECV  2     /* 机上 → レシーバー（Notify） */
int yd_sec_gatt_uuid(const uint8_t psk[YD_SEC_KEY_LEN], uint8_t kind, uint8_t out[YD_SEC_GATT_UUID_LEN]);

/* role が自分の確認コードを作る（失敗時は mac_out を 0 で埋めて 0 以外を返す） */
int yd_sec_auth_mac(const yd_sec_keys_t *k, uint8_t role, const uint8_t nc[YD_SEC_NONCE_LEN],
                    const uint8_t np[YD_SEC_NONCE_LEN], uint8_t mac_out[YD_SEC_MAC_LEN]);

/* 定数時間の比較（一致で true） */
bool yd_sec_equal(const uint8_t *a, const uint8_t *b, size_t n);

/* 暗号化: 平文を 0x80, 0x00… で YD_SEC_PAD_BLOCK の倍数に埋め（out_cap に収まる分まで）、
 * out に [T, ctr, 暗号文, tag] を書いて全体の長さを返す。入らないときは -1 */
int yd_sec_seal(const uint8_t key[YD_SEC_KEY_LEN], uint32_t ctr, const uint8_t *pt, size_t pt_len,
                uint8_t *out, size_t out_cap);

/* 暗号化したときの全体の長さ（seal と同じ計算）。入らないときは 0 */
size_t yd_sec_sealed_len(size_t pt_len, size_t out_cap);

/* 復号: 成功で埋め草を除いた平文の長さ、改ざん・形式不正で -1。ctr_out に通し番号。
 * pt_cap は埋めた後の長さ（in_len - YD_SEC_OVERHEAD）以上が必要 */
int yd_sec_open(const uint8_t key[YD_SEC_KEY_LEN], const uint8_t *in, size_t in_len,
                uint32_t *ctr_out, uint8_t *pt, size_t pt_cap);

/* 機密データの消去 */
void yd_sec_wipe(void *p, size_t n);

#ifdef __cplusplus
}
#endif
