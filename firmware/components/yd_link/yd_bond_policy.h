/*
 * BLE ボンド（ペアリング情報）の扱いの決まり。NimBLE に依存しない形にして、ホストでテストする
 * （firmware/tests/host/test_bond_policy.c）。
 */
#pragma once
#include <stdbool.h>

typedef struct {
    bool peer_known;          /* 暗号化まで進み、相手のアドレスが分かっている */
    bool bonded_before_conn;  /* この接続より前から、この相手のボンドがあった */
    bool authed;              /* この接続で PSK の相互認証が通った */
    bool key_mismatch;        /* 相手の AUTH が合わなかった（相手の PSK が違う）ので、こちらから切った */
} yd_bond_conn_t;

/*
 * 切断したときに、相手のボンドを消すか。
 *  - 認証が通った相手のボンドは消さない
 *  - この接続で新しくできたボンドは、認証まで進まずに切れたら理由を問わず消す（相手から切った場合も）。
 *    鍵を持たない機器が、ペアリングだけして自分から切り、ボンドの枠を占有するのを防ぐ
 *  - 前からあったボンドは、相手の PSK が違うと確かめたときだけ消す。時間切れ・不正なパケット・電波の都合では
 *    残す。前からのボンドを使って暗号化できた相手は、そのボンドを作った本物の相手なので、一時的な失敗で忘れると、
 *    相手側はボンドを持ったまま暗号化に失敗し続ける（2台とも、登録済みの相手のボンドは自分からは作り直さない）
 */
static inline bool yd_bond_forget_on_disconnect(const yd_bond_conn_t *c)
{
    if (!c->peer_known || c->authed) {
        return false;
    }
    return !c->bonded_before_conn || c->key_mismatch;
}

/*
 * 机上ユニットの受付時間中（起動後 120 秒）の再ペアリング（ボンドのある相手が、ペアリングし直そうとした）。
 * レシーバーのアドレスを名乗るだけで本物のボンドを消させられないように、古いボンドを RAM に退避してから消し、
 * その接続が PSK の相互認証を通らずに切れたら書き戻す。
 *
 * 退避は 1 件分だけ持つ。退避を使っている間（その接続が切れるまで。机上ユニットの接続は 1 本だけ）に来た
 * 2 回目の再ペアリングは断る。同じ接続の中で古いボンドを 2 回消させて、退避を上書きさせないため。
 */
typedef enum {
    YD_REPEAT_REFUSE,           /* 断る（ボンドは消さない） */
    YD_REPEAT_STASH_AND_RETRY,  /* 古いボンドを退避してから消し、ペアリングし直させる */
} yd_repeat_t;

static inline yd_repeat_t yd_bond_on_repeat_pairing(bool window_open, bool stash_in_use)
{
    return window_open && !stash_in_use ? YD_REPEAT_STASH_AND_RETRY : YD_REPEAT_REFUSE;
}

typedef enum {
    YD_STASH_NOTHING,           /* 退避していない */
    YD_STASH_RESTORE,           /* この接続で作ったボンドを消し、退避したボンドを書き戻す */
    YD_STASH_DISCARD,           /* 再ペアリングした相手が相互認証を通った（本当にボンドをなくした本物）。退避を捨てる */
} yd_stash_action_t;

/*
 * 接続が切れたときの退避の扱い。stash_from_this_conn: 退避したのが、いま切れた接続か。
 * 退避した接続が相互認証を通ったときだけ捨て、それ以外は書き戻す（退避が前の接続のものとして残っていたら、
 * その接続は認証を通っていないので、この接続で何があっても書き戻す）
 */
static inline yd_stash_action_t yd_bond_stash_on_disconnect(bool stash_in_use, bool stash_from_this_conn, bool authed)
{
    if (!stash_in_use) {
        return YD_STASH_NOTHING;
    }
    return stash_from_this_conn && authed ? YD_STASH_DISCARD : YD_STASH_RESTORE;
}
