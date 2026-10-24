/* otp_ctl_filter.h: レシーバーから届いた SET_REPORT(Feature) のうち、YubiKey に出してよいもの */
#include <stdio.h>
#include <string.h>
#include "otp_ctl_filter.h"

static int s_fail, s_pass;
#define CHECK(cond, msg) do { if (cond) { s_pass++; } else { s_fail++; printf("FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)

/* フレーム（データ 64 + コマンド + CRC 2 + 埋め 3）の番号 seq のレポート。ykman / ykpers と同じ形 */
static void report(uint8_t out[8], uint8_t cmd, int seq)
{
    uint8_t frame[70];
    memset(frame, 0x5A, 64);
    frame[64] = cmd;
    frame[65] = 0x12;
    frame[66] = 0x34;
    memset(frame + 67, 0, 3);
    memcpy(out, frame + seq * 7, 7);
    out[7] = (uint8_t)(0x80 | seq);
}

static bool allowed(uint8_t cmd, int seq)
{
    uint8_t r[8], c;
    report(r, cmd, seq);
    return otpcf_set_allowed(r, &c);
}

int main(void)
{
    static const uint8_t ok_cmds[] = { 0x10, 0x13, 0x20, 0x28, 0x30, 0x38 };
    /* スロットの書き込み・更新・入れ替え、NDEF、デバイスの設定、スキャンマップ、デバイス情報の書き込み、知らないもの */
    static const uint8_t bad_cmds[] = { 0x01, 0x03, 0x04, 0x05, 0x06, 0x08, 0x09, 0x11, 0x12, 0x15, 0x00, 0x02, 0x14, 0x31, 0xFF };
    uint8_t r[8], c;

    for (size_t i = 0; i < sizeof(ok_cmds); i++) {
        for (int seq = 0; seq <= 9; seq++) {
            CHECK(allowed(ok_cmds[i], seq), "challenge-response and read-only commands pass in every report");
        }
    }
    for (size_t i = 0; i < sizeof(bad_cmds); i++) {
        for (int seq = 0; seq < 9; seq++) {
            CHECK(allowed(bad_cmds[i], seq), "reports before the last one only fill the key's buffer");
        }
        CHECK(!allowed(bad_cmds[i], 9), "the last report of a command that changes the key is blocked");
    }
    /* 0x00〜0xFF の全部: 最後のレポートで通るのは、許可した 6 つだけ（許可したもの以外は止める） */
    int passed = 0;
    for (int cmd = 0; cmd <= 0xFF; cmd++) {
        bool expect = false;
        for (size_t i = 0; i < sizeof(ok_cmds); i++) {
            expect |= cmd == ok_cmds[i];
        }
        bool got = allowed((uint8_t)cmd, 9);
        passed += got;
        if (got != expect) {
            printf("command 0x%02x: got %d expected %d\n", cmd, got, expect);
        }
        CHECK(got == expect, "only the allowed commands pass the last report");
    }
    CHECK(passed == (int)sizeof(ok_cmds), "exactly six commands pass");

    /* 8 バイト目の全部の値: 書き込み中の印と番号 0〜9、ダミーの書き込み以外は止める（変更するコマンドで試す） */
    for (int sb = 0; sb <= 0xFF; sb++) {
        report(r, 0x01, 9);
        r[7] = (uint8_t)sb;
        bool expect = sb == 0x8F || (sb >= 0x80 && sb <= 0x88);
        CHECK(otpcf_set_allowed(r, &c) == expect, "the sequence byte: only 0x80-0x88 and the dummy write 0x8F pass with a blocked command");
    }

    report(r, 0x01, 9);
    CHECK(!otpcf_set_allowed(r, &c) && c == 0x01, "the blocked command is reported for the log");

    memset(r, 0, sizeof(r));
    r[7] = 0x8F;
    CHECK(otpcf_set_allowed(r, &c), "the dummy write that resets the read state passes");
    r[7] = 0x8A;
    CHECK(!otpcf_set_allowed(r, &c), "a sequence number past the last report is blocked");
    r[7] = 0x09;
    CHECK(!otpcf_set_allowed(r, &c), "a report without the write flag is blocked");
    r[7] = 0xC9;
    CHECK(!otpcf_set_allowed(r, &c), "other flag bits are blocked");

    printf("otp_ctl_filter: %d passed, %d failed\n", s_pass, s_fail);
    return s_fail ? 1 : 0;
}
