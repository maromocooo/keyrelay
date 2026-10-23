# 開発メモ（ファームウェア）

> これは開発中に書き足していったメモで、経緯を残すために置いています。バイナリのファイル名
>（`desk_unit_v5_full.bin` など）、鍵のファイル名（`ydkey.bin`）、`--reader KeyRelay` などの手順は
> 当時のものです。いまの作り方は [BUILD.md](BUILD.md) を見てください。
> 各機能の設計（CCID・OTP・フィーチャーレポートの中継）と、期待されるログの例は今も参考になります。
> ログの例は日本語の設定（`idf.py menuconfig` → KeyRelay log language → Japanese）のものです。
> 既定のビルドでは、同じ内容が英語で出ます。


```
 Mac ══USB══ [レシーバー ESP32-S3] ──UART 3本── [机上ユニット ESP32-S3] ══USB══ YubiKey
      (FIDOキーに見える)                             (USBホスト)
```

- **receiver/**：Macに「FIDOセキュリティキー」として見えるHIDデバイス。Mac側の追加ソフトは不要
- **desk_unit/**：YubiKeyをUSBホストで掴み、FIDOの64バイトレポートをUARTと素通しで中継する
- **components/yd_link/**：両者のフレーム化リンク（チャンネル多重＋CRC16）。あとでBLEに差し替える前提

CTAPHIDの中身は解釈せずに素通しするので、YubiKey 4（U2F）でも5C NFC（FIDO2）でも、コードを変えずに動きます。

---

## 0. 開発環境（初回のみ）

ESP-IDF **v5.4.x** で動作確認済みです（ビルドは v5.4.4 で確認）。

```bash
mkdir -p ~/esp && cd ~/esp
git clone -b v5.4.4 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf && ./install.sh esp32s3
. ./export.sh        # ターミナルを開くたびに実行
```

レシーバーが使う `espressif/esp_tinyusb` は、初回ビルド時にコンポーネントマネージャが自動で取得します。

---

## 1. ボードの2つのUSB-Cの見分け方

Freenove ESP32-S3には、USB-Cが2つあります。

| ポート | 中身 | Macでの見え方（`ls /dev/cu.*`） | このファームでの用途 |
|---|---|---|---|
| **USB（ネイティブ）** | ESP32-S3内蔵USB | `cu.usbmodem*` | 書き込み／レシーバーではMacへのFIDO接続 |
| **COM（UART）** | USB-UART変換IC | `cu.usbserial*` / `cu.wchusbserial*` | ログ表示（monitor） |

- COM側が `/dev` に出てこない場合は、USB-UART変換IC（WCH CH34x系）のmacOSドライバを入れてください
- **書き込みはネイティブ側（`cu.usbmodem*`）が確実**です。ドライバ不要で書き込めます

> ⚠️ ファームを書き込んだ後は、ネイティブUSBがファームに使われるので自動では書き込みモードに入りません。
> 再書き込みするときは、**BOOTボタンを押しながらRSTを押して離す**と、ネイティブ側が書き込みポートとして復活します。

---

## 2. 配線

### 机上ユニット ↔ USB-Aメス基板（YubiKey 4を挿す）

| ESP32-S3 | USB-A基板 |
|---|---|
| 5V | VBUS |
| GND | GND |
| GPIO20 | D+ |
| GPIO19 | D- |

- 机上ユニットの**ネイティブUSB-Cには何も挿さない**でください（GPIO19/20と同じ線につながっています）
- 給電とログは、机上ユニットの**COM側USB-C**から取ります

### 机上ユニット ↔ レシーバー（UART、ステップ③で配線）

| 机上ユニット | レシーバー |
|---|---|
| GPIO17 (TX) | GPIO18 (RX) |
| GPIO18 (RX) | GPIO17 (TX) |
| GND | GND |

TXとRXは**クロス**させます。

---

## 3. ビルドと書き込み

```bash
cd desk_unit            # または receiver
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/cu.usbmodemXXXX flash     # ネイティブ側で書き込み
idf.py -p /dev/cu.usbserialXXXX monitor  # COM側でログを見る（Ctrl+] で終了）
```

---

## 4. 試験手順

### ステップ① 机上ユニット単体：YubiKeyを認識できるか

1. 机上ユニットに `desk_unit` を書き込み、USB-A基板を配線して YubiKey 4 を挿す
2. COM側でmonitorを開き、次のようなログが出ればOKです

```
I desk: ==== USBデバイス接続 VID=1050 PID=0407 ====
I desk: itf0: class=0x03 proto=1 ... → HID(キーボード=OTP)
I desk: itf1: class=0x03 proto=0 ... → HID
I desk: → FIDO HID を発見: itf1 EP IN=0x84 OUT=0x04 MPS=64
I desk: itf2: class=0x0b ...       → CCID(スマートカード=PIV/OpenPGP)
I desk: セルフテストOK: CTAPHID v2, デバイスFW 4.x.x, caps=0x.. [WINK U2Fのみ ]
I desk: 中継開始。
```

（インターフェース番号やEPアドレスは個体によって変わります）

- 「FIDOインターフェースが見つかりません」と出たら、Macに直挿しして `ykman info` を実行し、USBでFIDO（U2F）が有効か確認してください
- **このタイミングで待機電流を測っておく**と、電池寿命の見積もりに使えます（テスターをVBUSに直列に入れる）

### ステップ② レシーバー単体：MacがFIDOキーとして認識するか

1. もう1枚に `receiver` を書き込み、**ネイティブ側USB-C**をMacに挿す（ログを見たい場合はCOM側も挿す）
2. ログに `Macに接続されました` と出ればOKです
3. Macで確認：`system_profiler SPUSBDataType | grep -A6 KeyRelay`（または「システム情報 → USB」）
4. ブラウザで https://webauthn.io を開いて Register を押すと、レシーバーのログに
   `Macから要求あり (CMD=0x86) だがYubiKey未接続` と出ます。ここまで来ればMac側は問題ありません

### ステップ③ 有線中継：webauthn.io で認証が通るか

1. UARTの3本を配線（TX/RXはクロス）
2. 机上ユニット（YubiKeyあり、COM側から給電）とレシーバー（ネイティブ側をMacへ）の両方に電源を入れる
3. https://webauthn.io の **Advanced Settings** で、YubiKey 4（U2Fのみ）向けに次のように設定する
   - Discoverable Credential: **Discouraged**
   - User Verification: **Discouraged**
4. ユーザー名を入れて **Register** → ブラウザの「セキュリティキーを使う」を選ぶ → **YubiKeyにタッチ**
5. 続けて **Authenticate** → タッチ → 成功すれば中継完成です 🎉

YubiKey 5C NFC に替えたら、Discoverable を Required にしてパスキーも試せます（ファームの変更は不要）。

---

## 5. トラブルシュート

| 症状 | 確認すること |
|---|---|
| 机上ユニットでYubiKeyが認識されない | D+/D- の入れ違い、USB-A基板のはんだ、5Vが出ているか（テスターで約4.7〜5V） |
| `CRC error` が頻発する | GNDの配線、ジャンパワイヤの接触。`LINK_BAUD` を 115200 に下げて切り分け |
| Macがレシーバーを認識しない | COM側に挿していないか（ネイティブ側に挿す） |
| Registerでタイムアウト | 机上ユニットのログに「中継開始」が出ているか、TX/RXのクロス |
| 書き込めない | BOOTを押しながらRST → ネイティブ側の `cu.usbmodem*` に書き込む |

---

## 6. 既知の制限（MVP）

- リンクは暗号化していません（机上の有線前提）。BLE化するときにLE Secure Connectionsのボンディングを入れます
- 中継しているのは FIDO、CCID（PIV/OpenPGP/OATH、v5〜。10章）、OTP キーボード（v6〜。11章）です
- USBのVID/PIDはEspressifの `0x303A:0x4004` を試作用に借りています。**人に配る場合は自前のPIDを取得**してください

---

## 7. BLE版（UARTの線なし）

`YD_USE_UART` を 0（デフォルト）にすると、ボード間はBLEになります。

- 机上ユニット = BLE Peripheral（アドバタイズして待つ）、レシーバー = Central（探して接続）
- 独自GATTサービス。LE Secure Connections でボンディングし、**暗号化されたリンクでしか中継しません**
- 机上ユニットは「まだ誰ともペアリングしていない」か「起動後120秒以内」だけ新規ペアリングを受け付けます
- レシーバーは最初にペアリングした机上ユニットだけに接続します
- 入出力のない機器同士なので BLE のペアリング自体は Just Works です。v4 からはその上に事前共有鍵（9章）による相互認証と暗号化を重ねたので、ペアリングの瞬間に割り込まれても中身は読めず、偽のデータも受け付けません

### ペアリングをやり直す
1. 両方のボードで **BOOTボタンを3秒長押し**（ログに「ペアリング情報を消去しました」と出る）
2. 両方をRST（または抜き挿し）。2分以内に自動で再ペアリングされます

### 机上ユニットのBOOTボタン
- 短押し：USBバス停止／再開（電流測定用）
- 3秒長押し：ペアリング情報の消去

---

## 8. 省電力版（v3）：YubiKeyの自動サスペンド＋BLE/ESP32の省電力

机上ユニット（電池駆動側）だけの変更です。レシーバーは USB 給電なので常時起きたままです（BLEの共通コード更新のため、レシーバーも v3 を書き込んでください）。

| 書き込むファイル | 書き込み先 |
|---|---|
| `desk_unit_v3_full.bin` | 机上ユニット、オフセット `0x0` |
| `receiver_v3_full.bin` | レシーバー、オフセット `0x0` |

```bash
python -m esptool --chip esp32s3 -p /dev/cu.usbmodemXXXX write_flash 0x0 desk_unit_v3_full.bin
```

> `sdkconfig.defaults` が変わっています。自分でビルドする場合は、**`desk_unit/sdkconfig` を一度消してから** `idf.py build` してください（古い sdkconfig が残っていると省電力の設定が入りません）。

### 8.1 何が変わったか

**① YubiKey の自動サスペンド**

- 無通信が **5秒**（`IDLE_SUSPEND_MS`）続き、処理中の CTAPHID トランザクションがなければ、USBバスを止めます（YubiKey 12.2mA → 0.3mA）
  - 「処理中」= Macの要求を送ってから、最後の応答（KEEPALIVE 以外）を返し終わるまで。タッチ待ちで KEEPALIVE が流れている間も止めません
- Mac から要求が来たら、キューに積んでバスを再開 → YubiKey の再認識（約0.3〜0.5秒）→ 順番に流します。3秒以内に認識されなければ失敗としてMacにエラーを返し、レシーバーに「取り外し」を通知します
- 意図的なサスペンドはレシーバーに「取り外し」と伝えません（MacからはFIDOキーが挿さったままに見えます）

**② CTAPHID チャンネルIDの仮想化**

バス再開は実質「再列挙」なので、YubiKey は Mac が持っているチャンネルID（CID）を忘れます。そこで机上ユニットが間に入ります。

- Mac の `CTAPHID_INIT`（ブロードキャスト）には**机上ユニットが自分で答え**、仮想CIDを払い出します（YubiKeyは起こしません）。応答のバージョンや capabilities は、接続時のセルフテストで YubiKey から取得してキャッシュした値です
- 仮想CIDで最初の要求が来たとき、机上ユニットが YubiKey に自前の INIT を送って本物のCIDを取得し、以後は全パケットのCIDを相互に書き換えます
- 再列挙のたびに epoch を進め、本物のCIDはすべて取り直します（Mac側のCIDはそのまま使い続けられる）
- 仮想CIDは最大8個（LRUで入れ替え）。机上ユニット再起動前のCIDで要求が来た場合も、そのまま引き継ぎます

**③ BLE / ESP32 の省電力**

- 自動ライトスリープ＋DFS（CPU 40〜160MHz）。ESP-IDF の `examples/bluetooth/nimble/power_save` の `sdkconfig.40m.esp32s3` に準拠
  - Freenove / XIAO ESP32S3 には32kHz水晶がないので、BLEの低電力クロックは**メイン水晶**（ライトスリープ中も水晶は動かしたまま）
- USBバス稼働中は `ESP_PM_NO_LIGHT_SLEEP` と `ESP_PM_APB_FREQ_MAX` のロックを保持し、バス停止中だけ解放します（USBホストはクロックが止まると動かないため）
- BLE接続パラメータを机上ユニットから切り替えます（レシーバーはそのまま承認）
  - 通信中：間隔 15〜30ms / レイテンシ 0 / タイムアウト 4秒
  - 無通信3秒後：間隔 30ms / レイテンシ 12 / タイムアウト 6秒（実質 約390msごとにだけ起きる）
- 10秒ごとに状態サマリーをログに出します
- UART版（`YD_USE_UART=1`）ではライトスリープを使いません（UART受信がスリープ中に取りこぼすため）

**BOOTボタン（机上ユニット）**

- 短押し（バス稼働中）：すぐにバスを停止（サスペンド電流の測定用）
- 短押し（停止中）：バスを再開し、**30秒間は自動停止しない**（稼働電流の測定用）
- 3秒長押し：ペアリング情報の消去（従来どおり）

### 8.2 期待されるログ（待機 → サスペンド → 復帰）

```
I desk: [状態] リンク=接続中 バス=ON(中継可) 接続パラメータ=30.00ms/lat0/4000ms(低遅延) PMロック=保持(スリープ禁止) vCID=1/8 epoch=3 最終通信から2s
I yd_ble: 接続パラメータ変更を要求: 省電力（30ms / latency 12 / 6s）
I yd_ble: 接続パラメータ更新完了（省電力）: interval=30.00ms latency=12 timeout=6000ms（実質の起床間隔 約390ms）
I desk: ■ USBバス停止（無通信）。YubiKeyはサスペンドに入ります
I desk: YubiKeyをサスペンドしました（意図的な停止なので、レシーバーには取り外しを通知しません）
I desk: ■ サスペンド完了。PMロック解放（ライトスリープ可）
I desk: [状態] リンク=接続中 バス=停止(サスペンド) 接続パラメータ=30.00ms/lat12/6000ms(省電力) PMロック=解放(ライトスリープ可) ...

（ここで webauthn.io の Authenticate を押す）
I desk: Mac→Key vCID=ffffffff CMD=0x86 len=8（バス停止中）
I desk: MacのINITに代答: 仮想CID=1a2b3c4d を割当
I yd_ble: 接続パラメータ変更を要求: 低遅延（15-30ms / latency 0 / 4s）
I desk: Mac→Key vCID=1a2b3c4d CMD=0x90 len=...（バス停止中）
I desk: ▶ USBバス再開（Macからの要求）。YubiKeyの再認識を待ちます
I desk: YubiKey再認識 VID=1050 PID=0407
I desk: セルフテストOK（epoch 5）
I desk: ▶ 復帰完了（バス再開から 4xxms）
I desk: YubiKey準備完了（待ち 7xxms）。キューの要求を流します
I desk: CID対応付け: vCID=1a2b3c4d ⇔ devCID=0000000b（epoch 5, 3ms）
I desk: Key→Mac vCID=1a2b3c4d KEEPALIVE status=2（タッチ待ちなど。バスは止めません）
I desk: Key→Mac devCID=0000000b→vCID=1a2b3c4d CMD=0x90 len=...（完了）
I yd_ble: 接続パラメータ更新完了（低遅延）: interval=30.00ms latency=0 ...
（5秒後にまたバス停止）
```

レシーバー側には `机上ユニットから接続パラメータ変更要求: ... → 承認` と `接続パラメータ更新:` が出ます。サスペンドのたびに `YubiKey 未接続` が出ないことも確認してください。

### 8.3 試験手順

1. 両方に v3 を書き込み、BLEで接続させる（ペアリング済みならそのまま）
2. 机上ユニットのログで、接続から約3秒後に「接続パラメータ更新完了（省電力）」、約5秒後に「USBバス停止（無通信）」が出ることを確認
3. **10秒ほど放置**してから、webauthn.io で **Authenticate**（または Register）→ YubiKeyにタッチ → 成功すること
   - 放置後の最初の要求だけ、**0.5〜1秒ほど余計に待たされます**（BLEの省電力パラメータ ≦0.4秒 ＋ YubiKey再列挙 約0.5秒）
4. 成功後、ログで約5秒後に再びバス停止することを確認
5. 続けてもう一度 Authenticate（停止中でも、前回払い出した仮想CIDのまま通ること＝CID仮想化の確認）
6. 電流測定：テスターを机上ユニットの給電に直列に入れ、停止中と稼働中を比較。BOOT短押しで停止／再開（再開後30秒は稼働のまま）
7. 抜き挿し：バス停止中にYubiKeyを抜いて Authenticate → 約3秒後にMacにエラーが返り、レシーバーに「YubiKey 未接続」が出る。挿し直すと「中継開始」

### 8.4 注意・既知の制限

- 停止中にYubiKeyを抜いても、次の要求までは気づけません（バスが止まっているため）
- YubiKeyが挿さっていない間は、挿し込みを検出するためにバスを給電したままにします（その間はライトスリープしません）
- ライトスリープ中はUARTログが一瞬止まることがあります（壊れはしません）。10秒ごとの状態ログで様子を見てください
- 動作がおかしいときの切り分け：`main.c` の `IDLE_SUSPEND_MS` を大きくする（自動停止を事実上無効化）／`sdkconfig.defaults` の `CONFIG_PM_ENABLE` を外す（ライトスリープ無効化）

## 9. 事前共有鍵（v4）：BLEの割り込み対策

BLE の Just Works ペアリングは、ペアリングの瞬間に近くの攻撃者が間に入ると防げません。
v4 では2台に同じ 256bit の鍵（`ydkey.bin`）を書き込み、BLE の暗号化の上にもう一段の暗号化を重ねます。

- 接続のたびに両方が乱数を出し合い、鍵＋乱数からその接続だけの鍵を作る（HKDF-SHA256）
- 相手が同じ鍵を持っていることを互いに確認するまで、YubiKey のデータは1バイトも流さない
- データは AES-256-GCM。改ざん・古いデータの再送（リプレイ）を検知したら即切断
- 鍵が書き込まれていない機器は BLE 自体を起動しない
- 実装: `components/yd_link/yd_sec.c`（ESP-IDF 非依存。PC 上で Python の参照実装と一致を確認済み）

### 9.1 鍵を作る（Mac のターミナルで1回だけ）

```sh
(printf 'YDK1'; head -c 32 /dev/urandom) > ydkey.bin
ls -l ydkey.bin    # 36 バイトになっていればOK
```

- この1ファイルを **2台とも** に書き込みます。鍵は他人に渡さない・Git に入れないこと
- なくしても、新しく作って2台に書き直せば使えます

### 9.2 書き込み（esptool-js）

両方とも、ファームと鍵を **同時に** 書き込みます（Add File で2行にする）。

| アドレス | ファイル |
|---|---|
| `0x0` | `desk_unit_v4_full.bin` または `receiver_v4_full.bin` |
| `0x190000` | `ydkey.bin` |

- 鍵はファーム領域の外にあるので、以後のファーム更新（0x0 だけ書く）では消えません
- **Erase Flash をすると鍵も消えます**。その場合は鍵も書き直してください
- ペアリング情報（NVS）は v3 から引き継がれるので、再ペアリングは不要です

### 9.3 期待されるログ

```
I yd_ble: 鍵を読み込みました（0x190000）
I yd_ble: 暗号化 OK（bonded=1）
I yd_ble: ▶ BLEリンク確立（暗号化＋鍵による相互認証済み）
```

うまくいかないとき:

| ログ | 原因 |
|---|---|
| `鍵がありません。ydkey.bin を 0x190000 に…` | 鍵を書き込んでいない / Erase Flash で消えた |
| `パーティション "ydkey" がありません` | v3 以前のファーム（0x0 に v4 を書き込む） |
| `相手の鍵が一致しません` | 2台で別の ydkey.bin を書き込んでいる |
| `鍵による相互認証が時間内に終わりませんでした` | 片方が v3 以前のファーム |

### 9.4 守れないもの（SECURITY.md にも記載予定）

- 機器そのものを持ち去られた場合：フラッシュ暗号化は使っていないので、鍵は読み出せます（鍵を作り直せば無効化できる）
- レシーバーが挿さっている Mac 上のアプリからは、普通の USB セキュリティキーと同じように見えます（これは YubiKey を直接挿したときと同じ）
- YubiKey のタッチ（ユーザーの在席確認）はこれまでどおり必須です


## 10. スマートカード（CCID）中継（v5）：PIV / OpenPGP / OATH

Mac からはレシーバーが「FIDOキー」と「スマートカードリーダー（KeyRelay）」の2つに見える。
リーダーに挿さっているカードの正体は、机上ユニットにつないだ YubiKey の CCID インターフェース。
CCID メッセージ（最大3KB）を中身を解釈せずに丸ごと中継する（BLE上は200バイトずつ分割、暗号化は v4 と同じ）。

- **両方の書き換えが必要**（片方だけでも FIDO はそのまま動く。CCID は「カードなし」になる）
  - レシーバー: `receiver_v5_full.bin` を 0x0（COM側、いつもどおり）
  - 机上ユニット: `desk_unit_v5_full.bin` を 0x0。**今回だけはケースを開けて** B＋USB で書き込む。
    v5 からは下の「ケースを開けずに書き換え」が使えるので、次からは不要
  - どちらも Erase Flash はしない（0x190000 の鍵が消える）
- **USB給電版の既定では USB バスを自動停止しない**（`YD_BATTERY_POWERED 0`）。止めると YubiKey の
  スマートカードがリセットされ、PIN 入力済みの状態が消えるため。BLE も常に低遅延の接続パラメータ

### 10.1 試し方（Mac のターミナル）

```sh
# GnuPG からカードが見えるか（シリアル番号などが出る）
gpg --card-status

# macOS 標準のスマートカード機能から見えるか
system_profiler SPSmartCardsDataType
```

- ykman / Yubico Authenticator は、リーダー名に "Yubico YubiKey" とインターフェース名（FIDO/CCID）が
  ないと機種を決められず `Failed to connect to YubiKey`（DEBUG ログでは `PID must be provided for non-NFC connections`）
  になる。そのため公開版では使えない（README 参照）。GnuPG や macOS 標準の PIV は名前を見ない
- GnuPG（`gpg --card-status`）は scdaemon が PC/SC 経由でリーダーを使う。うまくいかないときは
  `~/.gnupg/scdaemon.conf` に `disable-ccid` を書き、`gpgconf --kill scdaemon`

### 10.2 期待されるログ

机上ユニット（YubiKey 認識時）:
```
I (...) ccid: CCID: dwProtocols=0x2 dwFeatures=0x000400fe dwMaxCCIDMessageLength=3072 bMaxCCIDBusySlots=1
I (...) ccid: → CCID（スマートカード）を取得: itf2 EP IN=0x82 OUT=0x02 MPS=64
I (...) ccid: Mac→Key PowerOn seq=0 len=0
I (...) ccid: Key→Mac 0x80 seq=0 len=23 status=0x00 err=0x00（3ms）
I (...) ccid: Mac→Key XfrBlock seq=1 len=12
I (...) ccid: Key→Mac 0x80 seq=1 len=2 status=0x00 err=0x00（4ms）
```
レシーバー:
```
I (...) recv: 机上ユニット: YubiKey 接続済み [FIDO CCID]
I (...) ccid: スマートカード: あり（YubiKeyのCCIDが使えます）
I (...) ccid: Mac→ PowerOn seq=0 len=0
I (...) ccid: →Mac 0x80 seq=0 len=23 status=0x00 err=0x00（45ms）
```
- `dwFeatures` が 0x000400fe 以外、`dwMaxCCIDMessageLength` が 3072 より大きいと警告が出る
  （レシーバーは YubiKey 5 と同じ値を Mac に名乗っているため）。出たらログを送ってください
- 応答に1秒以上かかると、レシーバーが Mac に「時間延長」を返して待ってもらう（`応答待ち ...ms` のログ）。
  タッチ待ち（OpenPGP のタッチポリシーなど）は YubiKey 自身の時間延長がそのまま中継される
- 30秒何も返ってこなければ失敗として返す

### 10.3 ケースを開けずに書き換え（v5〜、机上ユニット）

> 公開版では既定でオフです。`YD_UART_DOWNLOAD_CMD` を `1` にしてビルドしたときだけ使えます。

机上ユニットのログ用 UART（CP2102N）に `keyrelay-download` と1行送ると、ROM のダウンロードモードで
再起動する（BOOT ボタンを押したのと同じ状態）。

1. CP2102N の USB-C を Mac に挿す（机上ユニットの電源もここから取れる）
2. `screen /dev/cu.usbserial-XXXX 115200` を開き、`keyrelay-download` と打って Enter
   （起動ログに「このシリアルに "keyrelay-download" と1行送ってください」と出ているはず）
3. `Ctrl-A` → `K` → `y` で screen を閉じる
4. esptool-js で同じポート（CP2102N）に接続し、`desk_unit_vX_full.bin` を 0x0 に書き込む（Erase しない）
5. 書き込み後は自動では再起動しないので、USB-C を挿し直す

注意: これで「机上ユニットの CP2102N を挿したPCで動くソフト」なら、BOOT ボタンなしで
書き換え・読み出し（鍵 ydkey を含む）ができるようになる。普段は充電器につないで使うなら影響はない。
不要なら `YD_UART_DOWNLOAD_CMD` を 0 にしてビルドする。

### 10.4 既知の制限

- BLE が一瞬切れると Mac からはカードが抜かれたように見え、再接続後にカードの電源入れ直し（PIN 再入力）になる
- CCID の割り込みエンドポイント（カード挿抜通知）は YubiKey 側を読まず、レシーバーが
  「YubiKey の有無＋リンク状態」から自分で通知する
- 同時に処理できる要求は1つ（YubiKey と同じ）

## 11. OTP（キーボード）中継（v6）

YubiKey をタッチしたときに打たれる OTP（Yubico OTP / 静的パスワードなど）を、レシーバーがキーボードとして Mac に打つ。
レシーバーは「FIDOキー＋スマートカードリーダー＋キーボード」の3つに見える。

- **両方の書き換えが必要**: レシーバー `receiver_v6_full.bin`（COM側）、机上ユニット `desk_unit_v6_full.bin`
  （ケースを開けずに: 10.3 の `keyrelay-download`）。どちらも 0x0、Erase しない
- 片方だけ v6 でも FIDO / CCID はそのまま動く（OTP だけ動かない）

### 11.1 キーが押しっぱなしにならない工夫

キーボードのレポートは「押す」「離す」の対。BLE の遅れで「離す」が遅れると、Mac のキーリピートで同じ文字が
何個も入ってしまう。そこで:
- 机上ユニット: 全キーが離されたレポートまで溜め、押す〜離すをひとまとめ（最大24レポート）で送る
- レシーバー: まとまりの中は間を空けずに Mac へ流す。押したまま 150ms 続きが来なければ自分で「離す」を送る。
  リンク断・YubiKey 取り外しでも「離す」を送る

### 11.2 日本語入力のままでも化けないように（v6.1〜、レシーバー）

かな入力がオンだと `cccccc...` がローマ字変換されて化ける（YubiKey を直接挿しても同じ）。
レシーバーは、1秒以上間が空いてから打ち始めるときに、先に「英数」キー（HID の LANG2）を1回押す
（`YD_OTP_FORCE_EISU 1`）。打ち終わっても英数のまま。英語配列の Mac では無視される。

### 11.3 試し方

1. 初回、Mac に「キーボード設定アシスタント」が出たら、そのまま閉じてよい（OTP の文字はどの配列でも同じ）
2. テキストエディタを開いて、YubiKey の金色の丸を短くタッチ（スロット1）
3. `cccccc...` で始まる44文字くらいが1行入れば成功。
   https://demo.yubico.com/otp/verify に貼ると本物の OTP か確認できる

期待されるログ:
```
机上ユニット:  otp: → キーボード（OTP）を取得: itf0 EP IN=0x81 MPS=8
              otp: YubiKeyがキー入力を開始（タッチ）→ Macへ中継
レシーバー:    recv: 机上ユニット: YubiKey 接続済み [FIDO CCID]
              recv:   OTP（キーボード）も中継します
```

### 11.4 まだできないこと

- HID の「フィーチャーレポート」を使う機能（チャレンジレスポンス = KeePassXC など、ykpers 系ツールでのスロット設定）

## 12. チャレンジレスポンス・スロット設定（v7）

YubiKey のチャレンジレスポンス（KeePassXC のデータベース保護など）とスロット設定（`ykman otp`）は、
USB 接続ではキーボードの口への **HID フィーチャーレポート**（8バイトの GET_REPORT / SET_REPORT）で行う
（CCID 経由は YubiKey 側が USB では受け付けない。ykman も「USB では OTP 接続でのみ」としている）。

v7 では、Mac が出したフィーチャーレポートの読み書きを1回ずつ BLE 越しに YubiKey まで往復させる。
- レシーバー: キーボードの口を自前の HID ドライバー（`hid_dev.c`）にし、GET_REPORT の返事を保留して、
  机上ユニット経由で YubiKey の答えが届いてから返す（TinyUSB 標準の HID はその場で答える作りなのでできない）
- 机上ユニット: 届いた要求を制御転送でそのまま YubiKey に出す（`otp_host.c`）
- 中身（OTP のフレーム・CRC など）は解釈しない。ykman / ykpers は「回数」で待つので、1回あたり数十ms 遅くなっても動く
- v7.1: **FIDO の口でもフィーチャーレポートを受け付ける**。本物の YubiKey もそうで、KeePassXC が使う ykpers は
  macOS では FIDO の口に送る（キーボードの口を開くには「入力監視」の許可が要るため）。
  FIDO の口も自前ドライバー（`hid_dev.c`）になり、TinyUSB 標準の HID クラスは使わなくなった（`CONFIG_TINYUSB_HID_COUNT=0`）

### 12.1 ホスト側のツール

ykman・Yubico Authenticator・KeePassXC は **Yubico の USB ID の機器しか相手にしない**ので、
公開版ではこれらのツールからは見えない（README 参照）。KeyRelay 専用の USB ID が取れたら、
各ツールに対応を提案する予定。
