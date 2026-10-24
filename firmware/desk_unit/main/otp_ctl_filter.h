/*
 * otp_ctl_filter: レシーバーから届いた SET_REPORT(Feature) を YubiKey に出してよいか
 *
 * YubiKey の OTP の口は、70 バイトのフレーム（データ 64 + コマンド 1 + CRC 2 + 埋め 3）を、7 バイトずつ
 * 10 個のフィーチャーレポートに分けて受け取る。レポートの 8 バイト目は 0x80 | 番号（0〜9）。番号 9 の
 * レポートが届いたときにフレームを実行し、コマンドの 1 バイトはそのレポートの 2 バイト目（フレームの 64 バイト目）
 * にある（全部 0 の途中のレポートは送られないことがあるが、最後のレポートは必ず送られる）。
 *
 * レシーバーになりすました相手（PSK を盗んだ・盗まれたレシーバー）は、タッチなしで OTP の口にコマンドを出せる。
 * 読むだけのコマンドとチャレンジレスポンスは通し、スロットの書き込み・入れ替え・アクセスコードの設定・
 * デバイスの設定など、キーを書き換えるコマンドは最後のレポートを出さずに止める（SECURITY.md）。
 * NimBLE・ESP-IDF に依存しない形にして、ホストでテストする（firmware/tests/host/test_otp_ctl_filter.c）。
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define OTPCF_REPORT_LEN   8
#define OTPCF_SEQ_FLAG     0x80     /* レポートの 8 バイト目: 書き込み中の印 | 番号 */
#define OTPCF_SEQ_LAST     9
#define OTPCF_DUMMY_WRITE  0x8F     /* 読み出しの状態を戻すためのダミーの書き込み（実行されるコマンドはない） */

/*
 * コマンド（フレームの 64 バイト目）。許可したもの以外はすべて止める（知らない番号も止める）。
 *
 *   通す（キーを書き換えない）
 *     0x10 DEVICE_SERIAL        シリアル番号の読み出し
 *     0x13 YK4_CAPABILITIES     機能（デバイス情報）の読み出し
 *     0x20 / 0x28 CHAL_OTP1/2   Yubico OTP のチャレンジレスポンス（スロット 1 / 2）
 *     0x30 / 0x38 CHAL_HMAC1/2  HMAC-SHA1 のチャレンジレスポンス（スロット 1 / 2。KeePassXC など）
 *
 *   止める（主なもの。ykman / ykpers の定義から）
 *     0x01 / 0x03 CONFIG1/2     スロットの書き込み（設定が空なら削除。アクセスコードの設定・変更も同じコマンド）
 *     0x04 / 0x05 UPDATE1/2     スロットの設定の更新
 *     0x06 SWAP                 スロット 1 と 2 の入れ替え
 *     0x08 / 0x09 NDEF1/2       NDEF（NFC で読ませる URL など）の書き込み
 *     0x11 DEVICE_CONFIG        デバイスの設定（USB のモードの切り替えなど）
 *     0x12 SCAN_MAP             スキャンマップ（キー配列）の書き込み
 *     0x15 YK4_SET_DEVICE_INFO  デバイスの設定（使える口、設定のロックなど）
 *     0x02 NAV ほか、上の表にない番号すべて
 *
 * テスト（test_otp_ctl_filter.c）は 0x00〜0xFF の全部について、通るのが上の 6 つだけであることを確かめる。
 */
#define OTPCF_CMD_DEVICE_SERIAL     0x10
#define OTPCF_CMD_YK4_CAPABILITIES  0x13
#define OTPCF_CMD_CHAL_OTP1         0x20
#define OTPCF_CMD_CHAL_OTP2         0x28
#define OTPCF_CMD_CHAL_HMAC1        0x30
#define OTPCF_CMD_CHAL_HMAC2        0x38

static inline bool otpcf_cmd_allowed(uint8_t cmd)
{
    switch (cmd) {
    case OTPCF_CMD_DEVICE_SERIAL:
    case OTPCF_CMD_YK4_CAPABILITIES:
    case OTPCF_CMD_CHAL_OTP1:
    case OTPCF_CMD_CHAL_OTP2:
    case OTPCF_CMD_CHAL_HMAC1:
    case OTPCF_CMD_CHAL_HMAC2:
        return true;
    default:
        return false;       /* 上の表の「止める」もの、知らないもの */
    }
}

/* d（8 バイト）を YubiKey に出してよいか。止めたときは *cmd に止めたコマンドを入れる（ログ用） */
static inline bool otpcf_set_allowed(const uint8_t d[OTPCF_REPORT_LEN], uint8_t *cmd)
{
    uint8_t s = d[OTPCF_REPORT_LEN - 1];
    *cmd = 0;
    if (s == OTPCF_DUMMY_WRITE) {
        return true;
    }
    if ((s & 0xE0) != OTPCF_SEQ_FLAG || (s & 0x1F) > OTPCF_SEQ_LAST) {
        return false;       /* ykman / ykpers が送らない形 */
    }
    if ((s & 0x1F) != OTPCF_SEQ_LAST) {
        return true;        /* 途中のレポートは溜められるだけで、何も実行されない */
    }
    *cmd = d[1];
    return otpcf_cmd_allowed(d[1]);
}
