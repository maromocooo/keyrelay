"""
KeyRelay desk unit case for a USB-A security key (YubiKey 5 NFC etc.), USB-C powered.

  pip install manifold3d trimesh numpy
  python3 case_usba_key.py            # writes stl/keyrelay_usba_*.stl

All dimensions are in mm and were measured on the parts listed in docs/BUILD.md.
Measure your own parts and adjust the constants below if they differ.
(Comments below are in Japanese.)

----
KeyRelay 机上ユニット — USB-A キー版ケース（USB-C 給電）

電池・昇圧モジュール・電源スイッチをやめ、USB-UART 変換基板（CP2102N, POS-FB-001）の
USB-C を「電源＋ログ」の差し込み口として背面に出す。充電器に挿せば電源だけ、Mac に挿せばログも見える。

配置（上から見て）
  手前の列: 左側面のスロット → YubiKey → USB-A メス基板（キーの差し込み口）
  奥の列  : USB-UART 変換（USB-C が背面左の穴へ）→ USB-C プラグ基板 → XIAO（アンテナ端が右）
  アンテナのシール（37x17.5）は右側面の内側のくぼみに貼る（キーや USB の金属から離す）。
  470µF は足を曲げて XIAO の裏（下側）に寝かせる。XIAO は表を天井にテープで貼る。

使い方:  python3 case_usba_key.py   → stl/keyrelay_usba_body.stl / _cover.stl / _test_usbc_port.stl / _test_touch.stl
座標系（使用時の向き）: X=左→右, Y=手前→奥, Z=下→上。上面=タッチ面、底面=底蓋。
★印は未実測の値。
"""
import os
import manifold3d as m3d
from manifold3d import Manifold, CrossSection, JoinType

m3d.set_circular_segments(64)

VARIANT = os.environ.get("DOCK_VARIANT", "B").upper()
NOTCH_X = 9.0        # B: 角の切り欠きの幅（お尻〜キーリング穴を収める）
PROTRUDE = 7.0       # C: お尻を出す量

# ---------------- 全体 ----------------
WALL, TOP = 2.4, 2.0
R_OUT, CHAMFER, CLR = 6.0, 1.2, 0.3

# ---------------- XIAO + USB-C プラグ基板（奥の列、X方向に一直線） ----------------
XIAO_L, XIAO_W, XIAO_T = 21.0, 17.8, 4.6   # T は USB-C コネクタ込み（実測 4.5）
PLUG_L, PLUG_W = 24.1, 14.3                # ★ ADA-5978（プラグ先端込み）
PLUG_INSERT = XIAO_L + PLUG_L - 42.2       # 実測: XIAO の端〜プラグ基板の端 = 42.2mm

# ---------------- YubiKey 5 NFC（USB-A） ----------------
YK_L, YK_W, YK_T = 45.0, 18.0, 3.7   # 実測: 黒い本体の厚み 3.7mm
KEY_SEAT_ERR = 1.2           # 実機: USB-A 基板に挿したキーは、想定より 1.2mm 手前で止まる（お尻がはみ出した量）
KEY_SHIFT = 3.0              # 要望: キー全体を今より 3mm 奥に（お尻を外形の左端より引っ込める）
YK_PLUG = YK_L - (34.5 + KEY_SEAT_ERR)   # 実測 34.5mm ＋ 実機のずれ = 差し込み口から外に出ている長さ
YK_TOUCH_FROM_END = 18.7     # 実機: グレー本体（穴はお尻から 20.2mm）で円が差し込み口と反対へ 1.5mm → 18.7
YK_TOUCH_D = 11.5            # タッチ穴のキー面側（φ13 は大きすぎた → 11.5。金色の円 φ8 は全部見える）
# 表面側は 45° で広げる（天板を下にして刷るので、これ以上寝かせるとオーバーハング）
SLOT_CLR = 0.35              # 側面スロットのすき間
GRIP_R = 4.5                 # スロット両脇の指掛け（半円の切り欠き）

# ---------------- USB-A メス基板 ----------------
DAO_L, DAO_W = 22.7, 17.0    # 実測: 外形（レセプタクル込み）
DAO_RECEPT_H = 8.4           # 実測: 基板の裏〜レセプタクル上面 10.0 − 基板厚 1.6
DAO_RECEPT_W = 13.4          # 実測: レセプタクル（シェル）の幅
DAO_PCB_T = 1.6
REC_FRONT_EXTRA = 1.0        # 実機: 口のふちがくぼみの端に乗っていた
REC_RECESS = 1.2             # レセプタクル上面を天板に沈める深さ（残り 0.8mm。0.4mm は薄すぎて穴があいた）
DAO_ABOVE_KEY = 1.7          # 実測: キーを挿した状態で「レセプタクル上面 − キー上面」 (6.2 − 0.8 − 3.7)

# ---------------- USB-UART 変換（電源＋ログの USB-C） ----------------
MOD_L, MOD_W, MOD_PCB = 21.5, 12.0, 2.0   # 公称 20 だが実機の試し刷りで 1.5mm 足りず → 21.5。実測: 基板 2.0mm、基板の裏〜USB-C 上面 4.1mm
MOD_TOTAL = 4.1
USBC_W, USBC_H = 9.0, 3.2                 # USB-C の銀色の外枠（標準）。基板に半分沈むタイプ
USBC_OUT = 0.0                            # 実測: レセプタクル口は基板の端と揃っている
MOD_BELOW = 9.0                           # 基板の下に出るもの（ピンヘッダー＋巻いた線）
MOD_BACK_CLR = 0.45                        # 変換の手前の端と受けの壁のすき間（v3 の 0.3 から +0.15）
PORT_SKIN = 0.6                           # 背面の外側に残す薄い壁。基板の端はここで止まり、外からは USB-C の口だけ見える
CAP_D = 10.4                              # 470µF の缶（寝かせた状態の太さ）
ANT_L, ANT_H = 37.0, 17.5                 # 実測: アンテナのシール（線は 76mm）
ANT_POCKET = 0.5                          # 右側面の内側に掘るくぼみの深さ

# ---------------- 底蓋・磁石・爪 ----------------
MAG_D, MAG_T = 6.0, 2.0
BOSS_D, BOSS_INSET = 10.0, 4.6
COV_T = MAG_T + 0.6
RIM_H, RIM_W, RIM_CLR = 2.0, 1.4, 0.25
FOOT_D, FOOT_DEPTH = 8.0, 0.5
TAB_W, TAB_OUT, TAB_Z0, TAB_Z1 = 10.0, 1.4, 0.8, 1.8
RIB_T, RIB_H = 1.2, 4.0

# ---------------- レイアウト（内寸座標。外形座標 = 内寸 + WALL） ----------------
FRONT_Y = YK_W + 2.0 + (4.0 if VARIANT == "B" else 0.0)   # B は切り欠きの奥に壁を立てる分だけ深く
# 高さ: XIAO（4.6）＋その下に寝かせた 470µF（φ10.4）＋すき間。アンテナのシールが立つ高さも確保
IH = max(XIAO_T + 0.3 + CAP_D + 1.2, ANT_H + 0.4)
H_BODY = IH + TOP

# 奥の列（左から）: USB-UART 変換 → プラグ基板（線の出る端が左）→ XIAO（アンテナ端が右）
MOD_X0 = 3.6                               # 奥左の内角の丸みを避ける
MOD_X1 = MOD_X0 + MOD_W
MOD_CX = (MOD_X0 + MOD_X1) / 2
PLUG_X0 = MOD_X1 + 3.0                     # プラグ基板の線を曲げるすき間
XIAO_X0 = PLUG_X0 + (42.2 - XIAO_L)        # 実測: XIAO の端〜プラグ基板の端 = 42.2mm
XIAO_X1 = XIAO_X0 + XIAO_L
PLUG_X1 = XIAO_X0 + PLUG_INSERT
XIAO_Y0 = FRONT_Y + CLR + RIB_T + 0.3
XIAO_Y1 = XIAO_Y0 + XIAO_W
XIAO_CY = (XIAO_Y0 + XIAO_Y1) / 2
IY = max(XIAO_Y1 + CLR + RIB_T + 0.8, FRONT_Y + MOD_L - (WALL - PORT_SKIN) + 1.0, ANT_L + 8.0)
MOD_Y1 = IY + WALL - PORT_SKIN            # 基板の端は壁の内側のくぼみに入り、外側 0.6mm の壁で止まる
MOD_Y0 = MOD_Y1 - MOD_L
USBC_TOP = IH - 0.05                       # USB-C の外枠が天井にほぼ接する（実機の試し刷りで確認）
USBC_CZ = USBC_TOP - USBC_H / 2
MOD_PCB_BOT = USBC_TOP - MOD_TOTAL
MOD_PCB_TOP = MOD_PCB_BOT + MOD_PCB

# 手前の列: 左側面から キー → USB-A メス基板 → 昇圧モジュール → スイッチ
YK_Y0 = 0.6
KEY_CY = YK_Y0 + YK_W / 2
YK_X0 = -WALL - (PROTRUDE if VARIANT == "C" else 0.0) + KEY_SHIFT - KEY_SEAT_ERR   # キー端（今の実機の位置から 3mm 奥）
YK_BODY_X1 = YK_X0 + (YK_L - YK_PLUG)
DAO_X0 = YK_BODY_X1                 # レセプタクル開口
DAO_X1 = DAO_X0 + DAO_L
REC_TOP = IH + REC_RECESS
IX = max(YK_X0 + (YK_L - YK_PLUG) + DAO_L + 1.6 + 1.0, XIAO_X1 + 5.0)   # XIAO の右に U.FL の線の曲げしろ
OX, OY = IX + 2 * WALL, IY + 2 * WALL
KEY_TOP = REC_TOP - DAO_ABOVE_KEY
KEY_BOT = KEY_TOP - YK_T
DAO_Z0 = REC_TOP - DAO_RECEPT_H - DAO_PCB_T
TOUCH_X = YK_X0 + YK_TOUCH_FROM_END


def rrect(x, y, r):
    r = max(r, 0.01)
    return CrossSection.square([x - 2 * r, y - 2 * r]).translate([r, r]).offset(r, JoinType.Round)


def box(x0, y0, z0, x1, y1, z1):
    return Manifold.cube([x1 - x0, y1 - y0, z1 - z0]).translate([x0, y0, z0])


def cyl(cx, cy, z0, z1, d, d_top=None):
    return Manifold.cylinder(z1 - z0, d / 2, (d_top or d) / 2).translate([cx, cy, z0])


def I(x, y):
    return x + WALL, y + WALL


def Ibox(x0, y0, z0, x1, y1, z1):
    a, b = I(x0, y0)
    c, d = I(x1, y1)
    return box(a, b, z0, c, d, z1)


# 磁石: 手前2か所。左手前はキーの下に収まる低いボス（キーがその上を通る）
_LB_X = (NOTCH_X - WALL + BOSS_INSET + 1.0) if VARIANT == "B" else BOSS_INSET
BOSS_CENTERS = [I(_LB_X, BOSS_INSET), I(IX - 12.0, BOSS_INSET)]   # 右は右側面のアンテナを避けて内側へ
SLOT_WALL_X0 = NOTCH_X if VARIANT == "B" else 0.0      # キースロットのある壁の外面（外形座標）
NOTCH_Y = I(0, YK_Y0 + YK_W + SLOT_CLR + 1.2)[1]         # B: 切り欠きの奥行き（外形座標）
RAIL_X0 = {"A": GRIP_R + 2, "B": NOTCH_X - WALL + 0.8, "C": 0.8}[VARIANT]   # キー受けレールの開始（内寸座標）


def rrect4(x0, y0, x1, y1, radii):
    """角ごとに半径を変えた角丸長方形（手前左, 手前右, 奥右, 奥左）"""
    pts = [(x0, y0), (x1, y0), (x1, y1), (x0, y1)]
    sg = [(1, 1), (-1, 1), (-1, -1), (1, -1)]
    circles = []
    for (px, py), (sx, sy), r in zip(pts, sg, radii):
        r = max(r, 0.05)
        circles.append(CrossSection.circle(r, 48).translate([px + sx * r, py + sy * r]))
    return CrossSection.batch_hull(circles)


def outline_pieces():
    """外形を凸な部品に分けたもの（面取りを hull で正確に作るため）"""
    if VARIANT != "B":
        return [rrect(OX, OY, R_OUT)]
    r = 3.0
    return [rrect4(NOTCH_X, 0, OX, OY, (r, R_OUT, R_OUT, R_OUT)),       # 切り欠きより右
            rrect4(0, NOTCH_Y, OX, OY, (r, R_OUT, R_OUT, R_OUT))]        # 切り欠きより奥


def outline():
    """外形（上から見た形）"""
    return CrossSection.batch_boolean(outline_pieces(), m3d.OpType.Add)


def cavity_cs():
    return outline().offset(-WALL, JoinType.Round)


def chamfered_prism(z0, z1, top=0.0, bot=0.0):
    """外形の柱に上下の面取りを付ける（凸な部品ごとに hull して結合）"""
    out = []
    for cs in outline_pieces():
        layers = [cs.extrude(z1 - z0 - top - bot).translate([0, 0, z0 + bot])]
        if top:
            layers.append(cs.offset(-top, JoinType.Round).extrude(0.01).translate([0, 0, z1 - 0.01]))
        if bot:
            layers.append(cs.offset(-bot, JoinType.Round).extrude(0.01).translate([0, 0, z0]))
        out.append(Manifold.batch_hull(layers))
    return Manifold.batch_boolean(out, m3d.OpType.Add)


BOSS_TOPS = [KEY_BOT - 1.0, DAO_Z0 - 2.5]
TAB_XS = [IX * 0.3, IX * 0.75]


def body():
    shell = chamfered_prism(0, H_BODY, top=CHAMFER)
    cavity = cavity_cs().extrude(IH + 1).translate([0, 0, -1])
    parts = [shell - cavity]

    for (cx, cy), zt in zip(BOSS_CENTERS, BOSS_TOPS):
        parts.append(cyl(cx, cy, 0, zt, BOSS_D))

    # XIAO のレール（手前と奥。天板から吊る）
    zr = IH - XIAO_T - 1.0
    parts.append(Ibox(XIAO_X0, XIAO_Y0 - CLR - RIB_T, zr, XIAO_X1, XIAO_Y0 - CLR, IH + .01))
    parts.append(Ibox(XIAO_X0, XIAO_Y1 + CLR, zr, XIAO_X1, XIAO_Y1 + CLR + RIB_T, IH + .01))
    # USB-UART 変換の左右のガイド（基板の下面より1mm下まで）
    for x0 in (MOD_X0 - CLR - RIB_T, MOD_X1 + CLR):
        parts.append(Ibox(x0, MOD_Y0 + 4, MOD_PCB_BOT - 1.0, x0 + RIB_T, IY + .01, IH + .01))
    # 変換の手前の端の受け（天板から下がる壁）。すき間 0.45mm: 基板を少し傾けて奥のくぼみに入れてから下ろす
    parts.append(Ibox(MOD_X0 + 1.0, MOD_Y0 - MOD_BACK_CLR - RIB_T, MOD_PCB_BOT - 0.5, MOD_X1 - 1.0, MOD_Y0 - MOD_BACK_CLR, IH + .01))   # 基板の厚み全体を受ける（元の長さ）

    # キーの通り道: 奥側のガイドリブ（手前は側壁）。キーは側面スロットとレセプタクルで支持
    parts.append(Ibox(RAIL_X0, YK_Y0 + YK_W + SLOT_CLR, KEY_BOT - 1.5, DAO_X0 - 0.5,
                      YK_Y0 + YK_W + SLOT_CLR + RIB_T, IH + .01))
    # キーの下に受けレール（左右のふち 1.5mm だけ下から支える。側面から差し込める）
    for y0, y1 in [(-0.01, YK_Y0 + 1.5),   # 手前のレールは前の壁につなげる（宙に浮かせない）
                   (YK_Y0 + YK_W - 1.5, YK_Y0 + YK_W + SLOT_CLR + RIB_T)]:
        parts.append(Ibox(RAIL_X0, y0, KEY_BOT - 1.5, DAO_X0 - 0.5, y1, KEY_BOT - SLOT_CLR))

    # USB-A メス基板: 天板から吊る両側の壁 + 奥(+X)のストッパー
    #   手前(-X)へは、天板の逃げ穴のふち（高さ1.2mm）がレセプタクル上端を引っ掛けて止める
    for sgn in (-1, 1):
        y = KEY_CY + sgn * (DAO_W / 2 + CLR)
        y0, y1 = (y, y + RIB_T) if sgn > 0 else (y - RIB_T, y)
        parts.append(Ibox(DAO_X0, y0, DAO_Z0 - 1.0, DAO_X1, y1, IH + .01))
    # 奥のストッパー: 配線を通すため真ん中を空け、両端の柱だけにする（基板の角を受ける）
    for ya, yb in ((KEY_CY - DAO_W / 2 - CLR - RIB_T, KEY_CY - DAO_W / 2 + 2.5),
                   (KEY_CY + DAO_W / 2 - 2.5, KEY_CY + DAO_W / 2 + CLR + RIB_T)):
        parts.append(Ibox(DAO_X1, ya, DAO_Z0 - 1.0, DAO_X1 + 1.6, yb, IH + .01))

    b = Manifold.batch_boolean(parts, m3d.OpType.Add)

    cuts = []
    for cx, cy in BOSS_CENTERS:
        cuts.append(cyl(cx, cy, -0.01, MAG_T + 0.1, MAG_D + 0.2))
    # レセプタクル上面の逃げ（天板に沈める）。基板を下から入れて押し上げる
    # v17: 差し込み口の先（口のふち）が乗り上げないよう、キー側へ 1mm 長く
    cuts.append(Ibox(DAO_X0 - REC_FRONT_EXTRA, KEY_CY - DAO_W / 2 - CLR, IH - 0.01, DAO_X1, KEY_CY + DAO_W / 2 + CLR, REC_TOP))
    # 左側面のキースロット（側面〜キー通路）
    cuts.append(box(-1, I(0, YK_Y0 - SLOT_CLR)[1], KEY_BOT - SLOT_CLR, SLOT_WALL_X0 + WALL + 4.0,
                    I(0, YK_Y0 + YK_W + SLOT_CLR)[1], KEY_TOP + SLOT_CLR))
    # キー通路の上（天板下面）にも逃げ: キー上面と天板の干渉を避ける
    if KEY_TOP + SLOT_CLR > IH:
        cuts.append(Ibox(-0.01, YK_Y0 - SLOT_CLR, IH - 0.01, DAO_X0, YK_Y0 + YK_W + SLOT_CLR, KEY_TOP + SLOT_CLR))
    # 指掛け: スロットの手前・奥の両脇に半円の切り欠き（キーの側面をつまめる）
    ky0 = I(0, YK_Y0 - SLOT_CLR)[1]
    ky1 = I(0, YK_Y0 + YK_W + SLOT_CLR)[1]
    if VARIANT == "A":
        for gy in (ky0, ky1):
            cuts.append(cyl(0, gy, KEY_BOT - 2.0, KEY_TOP + 2.0, 2 * GRIP_R))
    if VARIANT == "C":
        # 差し込み口のまわりに 0.8mm 沈めた角丸の縁取り（ポート風）
        m = 2.2
        bez = rrect((ky1 - ky0) + 2 * m, YK_T + 2 * SLOT_CLR + 2 * m, 1.8).extrude(0.8 + 0.01)
        bez = bez.rotate([0, -90, 0]).rotate([-90, 0, 0])
        bb = bez.bounding_box()
        bez = bez.translate([0.8 + 0.01 - bb[3], ky0 - m - bb[1], KEY_BOT - SLOT_CLR - m - bb[2]])
        cuts.append(bez)
    # タッチ窓（すり鉢）
    tx, ty = I(TOUCH_X, KEY_CY)
    cuts.append(cyl(tx, ty, KEY_TOP - 0.01, H_BODY + 0.01, YK_TOUCH_D, YK_TOUCH_D + 2 * (H_BODY - KEY_TOP)))
    # 背面の USB-C: 口だけ外まで貫通。基板の端は壁の内側のくぼみ（外側に 0.6mm 残す）に入って抜け止めになる
    rw, rh = USBC_W + 0.4, USBC_H + 0.4       # 少しゆるめ（接着で固定する）
    ph = rrect(rw, rh, rh / 2 - 0.01).extrude(WALL + 0.02).rotate([90, 0, 0])   # XZ 平面の角丸穴を Y 方向に
    bb = ph.bounding_box()
    px, py = I(MOD_CX, IY - 0.01)
    cuts.append(ph.translate([px - rw / 2 - bb[0], py - bb[1], USBC_CZ - rh / 2 - bb[2]]))
    cuts.append(Ibox(MOD_X0 - 0.2, IY - 0.01, MOD_PCB_BOT - 0.2, MOD_X1 + 0.2, MOD_Y1 + 0.05, min(MOD_PCB_TOP + 0.6, IH)))
    # アンテナのシールを貼る浅いくぼみ（右側面の内側。上端は天井まで）
    ay0 = (IY - ANT_L) / 2
    cuts.append(Ibox(IX - 0.01, ay0 - 0.3, IH - ANT_H - 0.3, IX + ANT_POCKET, ay0 + ANT_L + 0.3, IH + 0.01))
    # 底蓋の爪が入る溝（奥の内壁、下端0.6mmは残す）
    for x in TAB_XS:
        cuts.append(Ibox(x - TAB_W / 2 - 0.4, IY - 0.01, 0.6, x + TAB_W / 2 + 0.4, IY + TAB_OUT + 0.4, TAB_Z1 + 0.5))
    # 底蓋を外す指掛け（手前面の中央、下端）
    cuts.append(box(OX / 2 - 8, -1, -0.01, OX / 2 + 8, WALL + 0.5, 1.6))
    return b - Manifold.batch_boolean(cuts, m3d.OpType.Add)


def cover():
    plate = chamfered_prism(-COV_T, 0, bot=0.8)
    outer = cavity_cs().simplify(0.01).offset(-RIM_CLR, JoinType.Round).simplify(0.01)
    rim = (outer - outer.offset(-RIM_W, JoinType.Round).simplify(0.01)).extrude(RIM_H)
    for cx, cy in BOSS_CENTERS:
        rim = rim - cyl(cx, cy, -1, RIM_H + 1, BOSS_D + 1.0)
    rim = rim - box(OX / 2 - 9, 0, -1, OX / 2 + 9, WALL + 4, RIM_H + 1)
    # 右側面はアンテナのシールが下まで来るので縁を切る
    ay0 = (IY - ANT_L) / 2
    rim = rim - box(OX - WALL - 4, I(0, ay0 - 1)[1], -1, OX, I(0, ay0 + ANT_L + 1)[1], RIM_H + 1)
    cov = plate + rim

    for x in TAB_XS:
        a, b = I(x - TAB_W / 2, IY - RIM_CLR - RIM_W)
        cov = cov + box(a, b, TAB_Z0, a + TAB_W, I(0, IY + TAB_OUT)[1], TAB_Z1)
    cuts = [cyl(cx, cy, -MAG_T - 0.1, 0.01, MAG_D + 0.2) for cx, cy in BOSS_CENTERS]
    fx0 = (NOTCH_X + 8) if VARIANT == "B" else 14
    for fx, fy in [(fx0, 14), (OX - 14, 14), (14, OY - 12), (OX - 14, OY - 12)]:
        cuts.append(cyl(fx, fy, -COV_T - 0.01, -COV_T + FOOT_DEPTH, FOOT_D))
    return cov - Manifold.batch_boolean(cuts, m3d.OpType.Add)


def dummies():
    d = {}
    d["yubikey"] = Ibox(YK_X0, YK_Y0, KEY_BOT, YK_BODY_X1, YK_Y0 + YK_W, KEY_TOP) + \
        Ibox(YK_BODY_X1, KEY_CY - 6, KEY_BOT + 0.6, YK_BODY_X1 + YK_PLUG, KEY_CY + 6, KEY_TOP - 0.6)
    d["usba_board"] = Ibox(DAO_X0, KEY_CY - DAO_W / 2, DAO_Z0, DAO_X1, KEY_CY + DAO_W / 2, DAO_Z0 + DAO_PCB_T) + \
        (Ibox(DAO_X0, KEY_CY - DAO_RECEPT_W / 2, DAO_Z0 + DAO_PCB_T, DAO_X0 + 14, KEY_CY + DAO_RECEPT_W / 2, REC_TOP)
         - Ibox(DAO_X0 - 1, KEY_CY - 6.2, KEY_BOT + 0.3, DAO_X0 + 12.5, KEY_CY + 6.2, KEY_TOP - 0.3))
    d["usba_solder"] = Ibox(DAO_X1 - 4, KEY_CY - 6, DAO_Z0 - 1.5, DAO_X1 - 1, KEY_CY + 6, DAO_Z0)
    d["xiao"] = Ibox(XIAO_X0, XIAO_Y0, IH - XIAO_T, XIAO_X1, XIAO_Y1, IH)
    d["cap_folded"] = Ibox(XIAO_X0 + 2, XIAO_CY - CAP_D / 2, IH - XIAO_T - 0.3 - CAP_D,
                           XIAO_X1 - 1, XIAO_CY + CAP_D / 2, IH - XIAO_T - 0.3)
    d["usbc_plug"] = Ibox(PLUG_X0, XIAO_CY - PLUG_W / 2, IH - XIAO_T + 0.6, PLUG_X1, XIAO_CY + PLUG_W / 2, IH - XIAO_T + 3.7)
    d["uart_pcb"] = Ibox(MOD_X0, MOD_Y0, MOD_PCB_BOT, MOD_X1, MOD_Y1, MOD_PCB_TOP)
    d["uart_usbc"] = Ibox(MOD_CX - USBC_W / 2, MOD_Y1 - 7.3, USBC_TOP - USBC_H, MOD_CX + USBC_W / 2, MOD_Y1 + USBC_OUT, USBC_TOP)
    d["uart_pins"] = Ibox(MOD_X0 + 1, MOD_Y0 + 0.5, MOD_PCB_BOT - MOD_BELOW, MOD_X1 - 1, MOD_Y0 + 3.5, MOD_PCB_BOT)
    ay0 = (IY - ANT_L) / 2
    d["antenna"] = Ibox(IX - 0.2 + ANT_POCKET - 0.3, ay0, IH - ANT_H, IX + ANT_POCKET - 0.3, ay0 + ANT_L, IH)
    return d


if __name__ == "__main__":
    import os
    import numpy as np
    import trimesh
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "stl")
    os.makedirs(out, exist_ok=True)

    def save(man, name):
        mesh = man.to_mesh()
        tm = trimesh.Trimesh(np.asarray(mesh.vert_properties)[:, :3], np.asarray(mesh.tri_verts), process=False)
        tm.export(os.path.join(out, name))
        return tm

    b, c = body(), cover()
    v = VARIANT
    save(b.rotate([180, 0, 0]).translate([0, OY, H_BODY]), "keyrelay_usba_body.stl")
    save(c.translate([0, 0, COV_T]), "keyrelay_usba_cover.stl")
    # 背面の USB-C まわりだけの試し刷り（差し込み口の合い具合を見る）
    pcx, _ = I(MOD_CX, 0)
    coupon = b ^ box(pcx - 12, I(0, MOD_Y0 - MOD_BACK_CLR - RIB_T - 1.0)[1], USBC_CZ - 8, pcx + 12, OY + 1, H_BODY + 1)   # 手前の受けまで含める
    save(coupon.rotate([180, 0, 0]).translate([0, OY, H_BODY]), "keyrelay_usba_test_usbc_port.stl")
    tx, ty = I(TOUCH_X, KEY_CY)
    tc = b ^ box(0, 0, DAO_Z0 - 1.5, I(DAO_X1 + 2.0, 0)[0], ty + 14, H_BODY + 1)   # タッチ窓＋USB-A基板の取り付け部まで
    save(tc.rotate([180, 0, 0]).translate([0, 0, H_BODY]), "keyrelay_usba_test_touch.stl")
    print(f"[{v}] 外形: {OX:.1f} x {OY:.1f} x {H_BODY + COV_T:.1f} mm  (内寸 {IX:.1f} x {IY:.1f} x {IH:.1f})")
    tx, ty = I(TOUCH_X, KEY_CY)
    print(f"タッチ窓中心: X={tx:.1f}, Y={ty:.1f}  キー上面Z={KEY_TOP:.2f}  天板上面Z={H_BODY:.2f}  深さ={H_BODY - KEY_TOP:.2f}  表面側φ={YK_TOUCH_D + 2 * (H_BODY - KEY_TOP):.1f}")
    print(f"USB-A基板 X {DAO_X0:.1f}-{DAO_X1:.1f}, 変換 X {MOD_X0:.1f}-{MOD_X1:.1f} Y {MOD_Y0:.1f}-{MOD_Y1:.1f}, プラグ X {PLUG_X0:.1f}-, XIAO X {XIAO_X0:.1f}-{XIAO_X1:.1f} Y {XIAO_Y0:.1f}-{XIAO_Y1:.1f}, ボス {BOSS_CENTERS}")
    print("body", b.status(), "cover", c.status())
