/* yd_bond_policy.h: 切断時に BLE ボンドを消すかどうか、受付時間中の再ペアリングで退避したボンドをどうするかの、全組み合わせの表 */
#include <stdio.h>
#include "yd_bond_policy.h"

static int s_fail, s_n;

static void check(const char *name, bool known, bool bonded, bool authed, bool mismatch, bool expect)
{
    yd_bond_conn_t c = { known, bonded, authed, mismatch };
    bool got = yd_bond_forget_on_disconnect(&c);
    s_n++;
    if (got != expect) {
        s_fail++;
        printf("FAIL %s: got %d expected %d\n", name, got, expect);
    }
}

static void check_repeat(const char *name, bool window, bool in_use, yd_repeat_t expect)
{
    s_n++;
    if (yd_bond_on_repeat_pairing(window, in_use) != expect) {
        s_fail++;
        printf("FAIL %s\n", name);
    }
}

static void check_stash(const char *name, bool in_use, bool this_conn, bool authed, yd_stash_action_t expect)
{
    s_n++;
    if (yd_bond_stash_on_disconnect(in_use, this_conn, authed) != expect) {
        s_fail++;
        printf("FAIL %s\n", name);
    }
}

int main(void)
{
    /*                                                    known  bonded authed mismatch forget */
    check("not encrypted yet: nothing to delete",          false, false, false, false, false);
    check("not encrypted, mismatch: nothing to delete",    false, true,  false, true,  false);
    check("new pairing, peer left before auth",            true,  false, false, false, true);
    check("new pairing, handshake failed or timed out",    true,  false, false, false, true);
    check("new pairing, wrong key",                        true,  false, false, true,  true);
    check("new pairing, authenticated",                    true,  false, true,  false, false);
    check("old bond, dropped before auth (radio)",         true,  true,  false, false, false);
    check("old bond, handshake timed out or malformed",    true,  true,  false, false, false);
    check("old bond, wrong key",                           true,  true,  false, true,  true);
    check("old bond, authenticated, normal disconnect",    true,  true,  true,  false, false);
    /* authed と mismatch が両方立つことは実装上ないが、認証済みなら消さない側に倒す */
    check("authenticated wins over mismatch flag",         true,  false, true,  true,  false);
    check("authenticated wins (old bond)",                 true,  true,  true,  true,  false);
    /* 受付時間中の再ペアリング */
    check_repeat("window open, nothing stashed: stash and re-pair",  true,  false, YD_REPEAT_STASH_AND_RETRY);
    check_repeat("outside the window: refuse",                       false, false, YD_REPEAT_REFUSE);
    check_repeat("second re-pairing while stashed: refuse",          true,  true,  YD_REPEAT_REFUSE);
    check_repeat("outside the window, stashed: refuse",              false, true,  YD_REPEAT_REFUSE);

    /*                                                               in_use this   authed  action */
    check_stash("nothing stashed",                                   false, false, false, YD_STASH_NOTHING);
    check_stash("nothing stashed, authenticated",                    false, true,  true,  YD_STASH_NOTHING);
    check_stash("re-paired peer failed or left before auth: restore", true, true,  false, YD_STASH_RESTORE);
    check_stash("re-paired peer authenticated (real receiver)",      true,  true,  true,  YD_STASH_DISCARD);
    check_stash("stash left from another connection: restore",       true,  false, false, YD_STASH_RESTORE);
    check_stash("another peer authenticated: still restore",         true,  false, true,  YD_STASH_RESTORE);

    printf("test_bond_policy: %d/%d passed\n", s_n - s_fail, s_n);
    return s_fail ? 1 : 0;
}
