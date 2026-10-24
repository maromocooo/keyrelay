/*
 * kbd_filter: 机上ユニットから届いたキー入力を、Yubico OTP の形のものだけ打つ
 *
 * Yubico OTP は ModHex（c b d e f g h i j k l n r t u v）の 32〜64 文字（公開 ID 0〜32 文字 + OTP 32 文字）で、
 * 修飾キーはなく、最後に Enter が付くことがある。YubiKey は1文字ごとに「そのキーだけを押す」レポートと
 * 「全部離す」レポートを送る。PSK を盗んで机上ユニットになりすました相手が、短い単語と Enter
 * （コマンドの実行、ダイアログの既定ボタン）を打てないように、次だけを打つ。
 *   - 修飾キーなしで、キーを1つだけ押すレポートの、その ModHex の文字。1つの OTP で KBDF_MAX_CHARS 文字まで
 *   - 同じく Enter。その OTP の ModHex が KBDF_MIN_CHARS〜KBDF_MAX_CHARS 文字のときだけ、1回だけ
 * 受信側（hid_dev.c）は、打つキーを「押す」「全部離す」の2つのレポートにして打ち、机上ユニットの
 * 「全部離す」は使わない。同じレポートを送り続けてキーを押しっぱなしにし、パソコンのキーリピートで文字を
 * 増やすことはできず、打った文字の数は、ここで数えた数と必ず一致する。
 *
 * 1つの OTP（まとまり）は、キーを押すレポートの間が KBDF_GAP_MS 以下で続く入力。打った Enter でも終わり、
 * 次に押されたキーから次の OTP になる（すぐに2回触ったとき）。「全部離す」レポートは、まとまりを延ばさない
 * （机上ユニットは、タッチ1回分を決まった長さのパケット列で送り、空いた分を「全部離す」で埋めてくる）。
 * 形に合わないものが1つでもあったまとまりは、Enter が来ても、間が空くまで残りを打たない。
 * 受信側がキューあふれや USB 送信の失敗で打てなかった文字があったまとまりも同じ（kbdf_drop）。捨てた文字を数えたままだと、
 * キューを詰まらせて文字を捨てさせ、空いてから Enter だけを打たせられる。
 *
 * ESP-IDF に依存しない。PC 上でテストする（firmware/tests/host/test_kbd_filter.c）。
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define KBDF_REPORT_LEN  8
#define KBDF_MIN_CHARS   32
#define KBDF_MAX_CHARS   64
#define KBDF_GAP_MS      1000     /* キーを押すレポートの間がこれより空いたら、新しいまとまり（次のタッチ） */
#define KBDF_KEY_ENTER   0x28
#define KBDF_BLOCKED     (-1)     /* kbdf_key: 形に合わないので打たない */

typedef struct {
    int chars;            /* このまとまりで打った ModHex の文字数 */
    bool bad;             /* 形に合わない入力があった → 間が空くまで打たない */
    bool done;            /* 打った Enter で終わった → 次に押されたキーから次のまとまり */
    bool started;
    int64_t last_ms;      /* 最後にキーを押すレポートが届いた時刻（「全部離す」は数えない） */
} kbdf_t;

static inline void kbdf_init(kbdf_t *f)
{
    f->chars = 0;
    f->bad = false;
    f->done = false;
    f->started = false;
    f->last_ms = 0;
}

static inline bool kbdf_is_modhex(uint8_t k)
{
    switch (k) {
    case 0x05: case 0x06: case 0x07: case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C:   /* b c d e f g h i */
    case 0x0D: case 0x0E: case 0x0F: case 0x11: case 0x15: case 0x17: case 0x18: case 0x19:   /* j k l n r t u v */
        return true;
    default:
        return false;
    }
}

/* 届いた1つのレポートを見て、打つキー（HID の usage）を返す。0 = 打つものなし（「全部離す」）、
 * KBDF_BLOCKED = 形に合わないので打たない。now_ms は届いた時刻 */
static inline int kbdf_key(kbdf_t *f, const uint8_t r[KBDF_REPORT_LEN], int64_t now_ms)
{
    bool any = false;
    for (int i = 0; i < KBDF_REPORT_LEN; i++) {
        any |= (r[i] != 0);
    }
    if (!any) {
        return 0;                   /* 全部離す（まとまりの時刻は進めない） */
    }
    if (!f->started || now_ms - f->last_ms > KBDF_GAP_MS) {
        f->chars = 0;               /* 間が空いた: 新しいまとまり */
        f->bad = false;
        f->done = false;
        f->started = true;
    } else if (f->done && !f->bad) {
        f->chars = 0;               /* Enter で終わった OTP のすぐ後: 次の OTP */
        f->done = false;
    }
    f->last_ms = now_ms;
    if (f->bad) {
        return KBDF_BLOCKED;
    }
    uint8_t key = 0;
    int keys = 0;
    for (int i = 2; i < KBDF_REPORT_LEN; i++) {
        if (r[i]) {
            key = r[i];
            keys++;
        }
    }
    /* 修飾キー（Shift / Ctrl / Cmd …）、予約のバイト、同時押し、キーのないレポート */
    if (r[0] != 0 || r[1] != 0 || keys != 1) {
        f->bad = true;
        return KBDF_BLOCKED;
    }
    if (key == KBDF_KEY_ENTER) {
        if (f->chars >= KBDF_MIN_CHARS && f->chars <= KBDF_MAX_CHARS) {
            f->done = true;         /* この OTP はここまで */
            return key;
        }
        f->bad = true;              /* 短い単語＋Enter など */
        return KBDF_BLOCKED;
    }
    if (!kbdf_is_modhex(key) || f->chars >= KBDF_MAX_CHARS) {
        f->bad = true;
        return KBDF_BLOCKED;
    }
    f->chars++;
    return key;
}

/* kbdf_key が返したキーを打てなかった（受信側のキューが一杯で捨てた、Mac に送れなかった）ときに呼ぶ。数えた文字が打った文字より
 * 多くなるので、このまとまりは形に合わないものとして扱い、間が空くまで残りも Enter も打たない */
static inline void kbdf_drop(kbdf_t *f)
{
    f->bad = true;
}
