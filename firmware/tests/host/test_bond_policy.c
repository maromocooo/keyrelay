/* yd_bond_policy.h: 切断時に BLE ボンドを消すかどうかの、全組み合わせの表 */
#include <stdio.h>
#include "yd_bond_policy.h"

static int s_fail, s_n;

static void check(const char *name, bool known, bool bonded, bool authed, bool failed, bool expect)
{
    yd_bond_conn_t c = { known, bonded, authed, failed };
    bool got = yd_bond_forget_on_disconnect(&c);
    s_n++;
    if (got != expect) {
        s_fail++;
        printf("FAIL %s: got %d expected %d\n", name, got, expect);
    }
}

int main(void)
{
    /*                                                    known  bonded authed failed  forget */
    check("not encrypted yet: nothing to delete",          false, false, false, false, false);
    check("not encrypted, failed: nothing to delete",      false, true,  false, true,  false);
    check("new pairing, peer left before auth",            true,  false, false, false, true);
    check("new pairing, handshake failed",                 true,  false, false, true,  true);
    check("new pairing, authenticated",                    true,  false, true,  false, false);
    check("old bond, dropped before auth (radio)",         true,  true,  false, false, false);
    check("old bond, handshake failed (wrong key)",        true,  true,  false, true,  true);
    check("old bond, authenticated, normal disconnect",    true,  true,  true,  false, false);
    /* authed と failed が両方立つことは実装上ないが、認証済みなら消さない側に倒す */
    check("authenticated wins over failure flag",          true,  false, true,  true,  false);
    check("authenticated wins (old bond)",                 true,  true,  true,  true,  false);

    printf("test_bond_policy: %d/%d passed\n", s_n - s_fail, s_n);
    return s_fail ? 1 : 0;
}
