# KeyRelay（日本語）

**USB のセキュリティキーをワイヤレスで使うための自作キット。** USB-A のセキュリティキー
（YubiKey 5 NFC で動作確認）をキーボードの横の小さな箱に挿し、Bluetooth LE でパソコンに挿した
レシーバーまで中継します。キーには手元で触れ、パソコンは閉じて机の下に置いたままで使えます。

![ケースの完成予想図](../images/case_preview.png)

[English README](../../README.md)

> **状態:** 作者が毎日使っている個人用の構成を、そのまま公開したものです。macOS と YubiKey 5 NFC
>（ファームウェア 5.1.2）1本で確認しています。Windows / Linux は未確認です。
> セキュアブートとフラッシュ暗号化はまだ設定していません。機器はキーそのものと同じように扱い、
> 頼る前に [SECURITY.md](SECURITY.md) を読んでください。

## できること

| 機能 | 中継のしかた | 状態 |
|---|---|---|
| FIDO2 / U2F（パスキー、WebAuthn） | 64バイトの CTAPHID レポート | 使える: webauthn.io で登録・認証（Chrome） |
| OpenPGP（GnuPG） | CCID（スマートカード）のメッセージ | 使える: `gpg --card-status` |
| タッチで打たれる OTP | キーボードのレポート（タッチ1回分を決まった並びで送る。既定は Yubico OTP のキーだけ） | 使える: demo.yubico.com で Yubico OTP が有効と判定 |
| PIV・OATH | CCID（スマートカード）のメッセージ | 中継はする。下の Yubico のツールの注意を参照 |
| OTP のチャレンジレスポンス・スロット設定 | HID のフィーチャーレポートを1回ずつ往復 | 中継はする。下の Yubico のツールの注意を参照 |

まだ確かめていないもの: PIV / OpenPGP での署名、一般的な PC/SC のソフト（OpenSC など）からの PIV、
Chrome の `chrome://settings/securityKeys` でのパスキー管理。

### ykman・Yubico Authenticator・KeePassXC

これらのツールは、Yubico の USB ベンダー ID を持つ機器としか通信しません。KeyRelay は自前の USB ID を
使い、他社の名前や ID は名乗らないので、**今のところこれらのツールからは見えません**。そのため、
OATH のコード（Yubico Authenticator / ykman）、PIV の管理（ykman）、OTP のチャレンジレスポンス
（KeePassXC）は使えません。

pid.codes で専用の USB ID が取れたら、各ツールに対応してもらうよう提案する予定です。
- **ykman:** 名前が「Yubico YubiKey」でない PC/SC リーダー経由でも、機種の情報を読めるようにしてもらう
- **KeePassXC:** KeyRelay の ID を、対応機器の一覧に足してもらう

ブラウザ（FIDO）・GnuPG・OTP の入力は、そのまま使えます。

## しくみ

```
 パソコン ══USB══ レシーバー ─────── BLE ─────── 机上ユニット ══USB══ セキュリティキー
         （FIDO HID ＋ CCID リーダー      （ESP32-S3、           （XIAO ESP32S3、
           ＋ OTP キーボード）             ネイティブUSB）         USB ホスト）
```

- **机上ユニット**（Seeed XIAO ESP32S3）はキーの USB ホストになり、FIDO・CCID・OTP キーボードの
  3つのインターフェースを掴んで、中身を解釈せずに中継します（FIDO のチャンネル ID だけは、キーが
  再列挙されても切れないように仮想化しています）。
- **レシーバー**（ネイティブ USB を持つ ESP32-S3 ボード）は、パソコンから同じ3つに見えます。
- BLE は LE Secure Connections に加えて、事前共有鍵・使い捨ての X25519（前方秘匿性）・AES-256-GCM の
  アプリ層で守っています。何が守れて何が守れないかは [SECURITY.md](SECURITY.md) を読んでください。
- どちらも USB 給電です。机上ユニットは、背面に口を出した CP2102N（USB-UART 変換）の USB-C から
  電源とログを取ります。

## 作り方

部品・配線・ファームのビルド・鍵の書き込み・ケースの組み立ては **[BUILD.md](BUILD.md)** にあります。
ざっくり言うと、リポジトリの一番上で:

```sh
# ESP-IDF v5.4
(printf 'YDK1'; head -c 32 /dev/urandom) > keyrelay-key.bin   # 2台で同じ鍵
idf.py -C firmware/receiver  set-target esp32s3 build merge-bin -o keyrelay_receiver_full.bin
idf.py -C firmware/desk_unit set-target esp32s3 build merge-bin -o keyrelay_desk_unit_full.bin
# まとめたイメージを 0x0 に、keyrelay-key.bin を 0x190000 に書き込む（BUILD.md 参照）
```

開発中の詳しいメモ（各機能の設計と、期待されるログ）は [DEV_NOTES.md](DEV_NOTES.md) にあります。

## まだできないこと・今後

- USB ID: 既定のビルドは pid.codes の**テスト用** PID `0x1209:0x0001` を使っています（他と重なりうる）。
  pid.codes はテスト用 PID の機器を人に配ることを認めていないので、専用の PID が取れるまでは
  ビルド済みのファームを配りません。自分で使う分を、自分でビルドしてください。
  公開したので、pid.codes に専用の PID を申請する予定です。
- ケースは USB-A キー用だけです。USB-C キー版と電池版の机上ユニットは今後。
- 要求は同時に1つずつ（キー自体と同じ）。BLE が切れるとパソコンからはカードを抜いたように見え、
  PIN 入力済みの状態は消えます。
- シリアルのログは、既定では英語です。`idf.py menuconfig` → KeyRelay log language で日本語にできます。
- セキュアブート・フラッシュ暗号化・NVS 暗号化は、既定では設定していません。下書きの手順は
  [HARDENING.md](HARDENING.md) にあります（未検証）。

## ライセンス

- ファームウェア・ツール・ドキュメント: [MIT](../../LICENSE)
- ハードウェア（`hardware/` のケースの設計データ）: [CERN-OHL-P-2.0](../../hardware/LICENSE)

## 商標

KeyRelay は独立したプロジェクトで、Yubico AB とは関係がなく、承認や支援も受けていません。
YubiKey と Yubico は Yubico AB の商標です。その他の製品名は各社の商標で、互換性の説明のためだけに
使っています。
