/* Host test for kbd_filter.h (receiver: only Yubico-OTP-shaped key input is typed).
 * kbdf_key() returns the key the receiver types as one press + release, so each return value > 0 is
 * exactly one typed character. */
#include <stdio.h>
#include <string.h>
#include "kbd_filter.h"

static int s_fail, s_pass;
#define CHECK(cond, msg) do { if (cond) { s_pass++; } else { s_fail++; printf("FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)

static const char MODHEX[] = "cbdefghijklnrtuv";
static uint8_t usage_of(char c)
{
    return (uint8_t)(0x04 + (c - 'a'));   /* HID usage: a = 0x04 ... z = 0x1d */
}

/* Types `text` (+ Enter if enter) as press/release pairs 8 ms apart starting at *t, as a YubiKey does.
 * Returns how many characters the receiver types. */
static int type(kbdf_t *f, const char *text, int enter, int64_t *t, uint8_t mod)
{
    int typed = 0;
    uint8_t r[8];
    size_t n = strlen(text);
    for (size_t i = 0; i <= n; i++) {
        if (i == n && !enter) {
            break;
        }
        memset(r, 0, sizeof(r));
        r[0] = mod;
        r[2] = (i == n) ? KBDF_KEY_ENTER : usage_of(text[i]);
        typed += kbdf_key(f, r, *t) > 0;
        *t += 8;
        memset(r, 0, sizeof(r));
        if (kbdf_key(f, r, *t) != 0) {
            printf("a release report was not passed as 'nothing to type'\n");
            s_fail++;
        }
        *t += 8;
    }
    return typed;
}

static void otp(char *buf, int len)
{
    for (int i = 0; i < len; i++) {
        buf[i] = MODHEX[(i * 7 + 3) % 16];
    }
    buf[len] = 0;
}

/* sends the same report n times, 2 ms apart; returns how many characters are typed */
static int repeat(kbdf_t *f, const uint8_t r[8], int n, int64_t *t)
{
    int typed = 0;
    for (int i = 0; i < n; i++) {
        typed += kbdf_key(f, r, *t += 2) > 0;
    }
    return typed;
}

static uint32_t s_rng = 0x12345678u;
static uint32_t rnd(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

int main(void)
{
    kbdf_t f;
    int64_t t = 1000;
    char s[200];

    kbdf_init(&f);
    otp(s, 44);
    CHECK(type(&f, s, 1, &t, 0) == 45, "a 44-character Yubico OTP and Enter are typed");
    t += 3000;
    otp(s, 32);
    CHECK(type(&f, s, 1, &t, 0) == 33, "a 32-character OTP (no public ID) and Enter are typed");
    t += 3000;
    otp(s, 64);
    CHECK(type(&f, s, 1, &t, 0) == 65, "a 64-character OTP and Enter are typed");
    t += 3000;
    otp(s, 44);
    CHECK(type(&f, s, 0, &t, 0) == 44, "an OTP without Enter is typed");

    t += 3000;
    CHECK(type(&f, "cd", 1, &t, 0) == 2, "a short word: the letters pass, the Enter does not");
    t += 3000;
    otp(s, 31);
    CHECK(type(&f, s, 1, &t, 0) == 31, "31 characters + Enter: Enter is blocked");
    t += 3000;
    otp(s, 70);
    CHECK(type(&f, s, 1, &t, 0) == 64, "more than 64 characters: the rest and the Enter are blocked");
    t += 3000;
    CHECK(type(&f, "ma", 0, &t, 0) == 0, "non-ModHex letters (m, a) are blocked");
    t += 3000;
    otp(s, 44);
    CHECK(type(&f, s, 1, &t, 0x02) == 0, "anything with Shift is blocked");
    t += 3000;
    otp(s, 10);
    int a = type(&f, s, 0, &t, 0);
    CHECK(type(&f, "x", 0, &t, 0) == 0, "a non-ModHex key spoils the burst");
    otp(s, 40);
    CHECK(a == 10 && type(&f, s, 1, &t, 0) == 0, "the rest of a spoiled burst is not typed");
    t += 3000;
    otp(s, 44);
    CHECK(type(&f, s, 1, &t, 0) == 45, "the next touch (after a pause) starts fresh");
    otp(s, 44);
    CHECK(type(&f, s, 1, &t, 0) == 45, "an OTP right after an Enter (a quick second touch) is typed");
    CHECK(type(&f, "cd", 1, &t, 0) == 2, "after an Enter, a short word + Enter still has its Enter blocked");

    /* the desk unit fills the rest of a touch's packets with release reports: they must not keep the
     * burst open, or an OTP without Enter right after the padding would count as the same burst */
    kbdf_init(&f);
    t += 3000;
    otp(s, 44);
    CHECK(type(&f, s, 0, &t, 0) == 44, "an OTP without Enter is typed");
    uint8_t pad[8] = { 0 };
    for (int i = 0; i < 40; i++) {     /* 2 s of padding, one release every 50 ms */
        t += 50;
        kbdf_key(&f, pad, t);
    }
    t += 1100;
    CHECK(type(&f, s, 0, &t, 0) == 44, "a second OTP without Enter, 1.1 s after the last key, is typed");

    /* a spoiled burst is not reset by an Enter */
    kbdf_init(&f);
    t += 3000;
    CHECK(type(&f, "x", 0, &t, 0) == 0, "a non-ModHex key is blocked");
    otp(s, 40);
    CHECK(type(&f, s, 1, &t, 0) == 0, "ModHex + Enter right after it in the same burst is blocked too");
    otp(s, 44);
    CHECK(type(&f, s, 1, &t, 0) == 0, "and so is the next one without a pause");

    /* held keys and several keys in one report: the receiver types one character per accepted report,
     * so the count here is what the computer sees */
    kbdf_init(&f);
    t += 3000;
    uint8_t c6[8] = { 0, 0, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06 };
    CHECK(repeat(&f, c6, 6, &t) == 0, "a report with several keys (even the same one) is blocked");
    kbdf_init(&f);
    t += 3000;
    uint8_t c1[8] = { 0, 0, 0x06 };
    uint8_t en[8] = { 0, 0, KBDF_KEY_ENTER };
    int held = repeat(&f, c1, 32, &t);
    CHECK(held == 32 && kbdf_key(&f, en, t += 2) == KBDF_KEY_ENTER,
          "32 identical [c] reports count as 32 characters (typed as 32 c's), so the Enter may follow");
    kbdf_init(&f);
    t += 3000;
    const uint8_t word[] = { 0x07, 0x08, 0x0F, 0x08, 0x17, 0x08 };   /* d e l e t e */
    int typed = 0;
    for (int w = 0; w < 6; w++) {
        uint8_t r[8] = { 0, 0, word[w] };
        typed += repeat(&f, r, 6, &t);
        kbdf_key(&f, pad, t += 2);
    }
    CHECK(typed == 36 && kbdf_key(&f, en, t += 2) == KBDF_KEY_ENTER,
          "'delete' with each letter held for 6 reports is typed as 36 letters, not as 'delete'");
    kbdf_init(&f);
    t += 3000;
    typed = 0;
    for (int w = 0; w < 6; w++) {
        uint8_t r[8] = { 0, 0, word[w] };
        typed += kbdf_key(&f, r, t += 2) > 0;
        kbdf_key(&f, pad, t += 2);
    }
    CHECK(typed == 6 && kbdf_key(&f, en, t += 2) == KBDF_BLOCKED, "'delete' typed once: its Enter is blocked");
    uint8_t rsv[8] = { 0, 1 };
    kbdf_init(&f);
    CHECK(kbdf_key(&f, rsv, t += 3000) == KBDF_BLOCKED, "a report with only the reserved byte set is blocked");

    /* releases always pass as "nothing to type" */
    uint8_t zero[8] = { 0 };
    f.bad = true;
    CHECK(kbdf_key(&f, zero, t) == 0, "a release report is never blocked");

    /* Enter together with a letter in one report is not an OTP */
    kbdf_init(&f);
    t += 3000;
    otp(s, 40);
    type(&f, s, 0, &t, 0);
    uint8_t both[8] = { 0, 0, KBDF_KEY_ENTER, usage_of('c') };
    CHECK(kbdf_key(&f, both, t) == KBDF_BLOCKED, "Enter with another key in the same report is blocked");

    /* Random streams (proper OTPs mixed with junk and gaps around KBDF_GAP_MS): the typed output only
     * ever contains ModHex letters and Enter, at most 64 letters since the last Enter or pause, and an
     * Enter only after 32 to 64 of them. */
    int violations = 0, enters = 0, letters = 0;
    for (int run = 0; run < 3000; run++) {
        kbdf_init(&f);
        t = 0;
        int64_t last_nz = 0;
        int have_nz = 0, cnt = 0;
        for (int i = 0; i < 300; i++) {
            uint8_t r[8] = { 0 };
            uint32_t x = rnd() % 100;
            if (x < 35) {
                /* release */
            } else if (x < 85) {
                r[2] = usage_of(MODHEX[rnd() % 16]);
            } else if (x < 89) {
                r[2] = KBDF_KEY_ENTER;
            } else if (x < 92) {
                r[2] = usage_of(MODHEX[rnd() % 16]);
                r[2 + 1 + rnd() % 5] = usage_of(MODHEX[rnd() % 16]);
            } else if (x < 95) {
                r[0] = (uint8_t)(1u << (rnd() % 8));
                r[2] = usage_of(MODHEX[rnd() % 16]);
            } else if (x < 98) {
                r[2] = (uint8_t)(0x04 + rnd() % 0x60);
            } else {
                r[1] = (uint8_t)(1 + rnd() % 255);
            }
            uint32_t g = rnd() % 40;
            t += g == 0 ? 950 + rnd() % 100 : g == 1 ? 2000 : rnd() % 20;
            int nz = 0;
            for (int k = 0; k < 8; k++) {
                nz |= r[k] != 0;
            }
            if (nz) {
                if (!have_nz || t - last_nz > KBDF_GAP_MS) {
                    cnt = 0;
                }
                have_nz = 1;
                last_nz = t;
            }
            int k = kbdf_key(&f, r, t);
            if (k == KBDF_KEY_ENTER) {
                violations += cnt < KBDF_MIN_CHARS || cnt > KBDF_MAX_CHARS;
                enters++;
                cnt = 0;
            } else if (k > 0) {
                violations += !kbdf_is_modhex((uint8_t)k);
                cnt++;
                letters++;
                violations += cnt > KBDF_MAX_CHARS;
            } else if (k != 0 && k != KBDF_BLOCKED) {
                violations++;
            }
            if (!nz && k != 0) {
                violations++;
            }
            if (rnd() % 30 == 0) {      /* now and then a well-formed OTP, so Enters do get typed */
                int len = 32 + (int)(rnd() % 33);
                for (int c = 0; c < len; c++) {
                    s[c] = MODHEX[rnd() % 16];
                }
                s[len] = 0;
                t += 1100;
                have_nz = 0;
                int got = type(&f, s, 1, &t, 0);
                violations += got != len + 1;
                last_nz = t - 16;
                have_nz = 1;
                enters++;
                letters += len;
                cnt = 0;
            }
        }
    }
    printf("random streams: %d letters and %d Enters typed\n", letters, enters);
    CHECK(violations == 0, "random streams never type anything but OTP-shaped input");
    CHECK(enters > 1000 && letters > 10000, "the random test does type OTPs");

    printf("kbd_filter: %d passed, %d failed\n", s_pass, s_fail);
    return s_fail ? 1 : 0;
}
