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
    bool hs_failed;           /* 認証前に、こちらから失敗として切った（不一致・時間切れ・不正な HELLO/AUTH） */
} yd_bond_conn_t;

/*
 * 切断したときに、相手のボンドを消すか。
 *  - 認証が通った相手のボンドは消さない
 *  - 認証前の失敗でこちらから切った相手は、古いボンドでも消す（鍵が一致しない相手を残さない）
 *  - この接続で新しくできたボンドは、認証まで進まずに切れたら消す（相手から切った場合も）。
 *    鍵を持たない機器が、ペアリングだけして自分から切り、ボンドの枠を占有するのを防ぐ
 *  - 前からあったボンドは、認証前に電波の都合で切れただけなら残す（正規の相手を忘れない）
 */
static inline bool yd_bond_forget_on_disconnect(const yd_bond_conn_t *c)
{
    if (!c->peer_known || c->authed) {
        return false;
    }
    return c->hs_failed || !c->bonded_before_conn;
}
