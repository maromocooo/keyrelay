# KeyRelay の守りを固める

[English](../HARDENING.md)

> **状態: レシーバーの実機で確認済み。** レシーバー（Freenove ESP32-S3、チップ revision v0.2、esptool v5.6、
> 2026-10-24）でこの手順を最後まで行い、セキュアブート・Release モードのフラッシュ暗号化・NVS 暗号化・
> セキュアダウンロードモードで起動すること、フラッシュの読み出しが拒否されること、そのレシーバー越しに
> FIDO・OpenPGP・OTP が動くことを確かめました。机上ユニットも設定と手順は同じですが、実機ではまだ固めて
> いません。固めたビルドの設定は、2台とも CI でもビルドしています。
>
> eFuse の操作は**どれも元に戻せません**。間違えると、二度と起動しない基板や、二度と更新できない基板に
> なることがあります。**必ず予備の基板で最後まで一度試してください。** Espressif の公式ドキュメントが、
> このページより優先です:
> [Security Features Enablement Workflows](https://docs.espressif.com/projects/esp-idf/en/v5.4.4/esp32s3/security/security-features-enablement-workflows.html)、
> [Flash Encryption](https://docs.espressif.com/projects/esp-idf/en/v5.4/esp32s3/security/flash-encryption.html)、
> [Secure Boot v2](https://docs.espressif.com/projects/esp-idf/en/v5.4/esp32s3/security/secure-boot-v2.html)、
> [NVS Encryption](https://docs.espressif.com/projects/esp-idf/en/v5.4/esp32s3/api-reference/storage/nvs_encryption.html)。

## 何が守れて、何を失うか

何もしないと、機器を数分借りた人が、フラッシュからペアリング用の鍵（PSK）と BLE のペアリング情報を
読み出し、どちらの機器にもなりすませます。PIN を記録するように書き換えたファームを書き込むこともできます
（[SECURITY.md](SECURITY.md)）。この手順のあとは、次のようになります。

| 機能 | 守れるもの | 代わりに失うもの |
|---|---|---|
| フラッシュ暗号化（Release モード） | フラッシュを読み出しても暗号文だけ（PSK・アプリ・NVS の鍵） | 更新には、その機器の鍵で暗号化したイメージが要る（`make_images.sh`） |
| セキュアブート v2 | 自分の鍵で署名したファームしか動かない | 署名鍵の管理。なくすと二度と更新できない |
| NVS 暗号化 | BLE のペアリング情報、レシーバーが覚えた机上ユニット | 実質なし（鍵は最初の起動で自動で作られる） |
| セキュアダウンロードモード | USB / UART の ROM ローダーからフラッシュの読み出しや RAM でのコード実行ができない | esptool に `--no-stub` が要る。デバッグ用の読み出しもできない |
| JTAG の無効化 | デバッガーからの操作 | デバッガーが使えない |

フラッシュ暗号化の *Development* モードでは**足りません**。ROM のダウンロードモードから RAM に
コードを読み込ませると、そのコードは復号されたフラッシュを読めるからです。そのため、この手順では
Release モードとセキュアダウンロードモードを使います。

### 守れないもの

- **盗まれた機器をそのまま使われること。** 固めると PSK は機器から読み出せなくなりますが、盗まれた機器も
  KeyRelay の機器としてそのまま動き、あなたのほかの機器と通信できます。締め出すには、ほかの全部の機器の PSK を
  変えます（[SECURITY.md](SECURITY.md) の「機器をなくしたり、盗まれたりしたら」）。
- **チップへの物理的な攻撃。** ESP32-S3 にはグリッチの検出がありません。Espressif の
  [AR2026-005](https://documentation.espressif.com/AR2026-005_Security_Advisory_Concerning_AES_Key_Recovery_Using_Voltage_Fault_Injection_on%20ESP32-S3_EN.html)
  は、基板を開けて電源電圧のフォールト注入を行い、ESP32-S3 の汎用の AES アクセラレーターから AES-128 の鍵を
  取り出す方法を説明しています。KeyRelay はこのアクセラレーターを、接続ごとのリンクの鍵と NVS 暗号化に
  AES-256 で使います（フラッシュ暗号化は別の回路です）。ほかの Espressif のチップでは、フォールト注入で
  セキュアブートとフラッシュ暗号化が破られています（ESP32 の
  [AR2023-005](https://documentation.espressif.com/AR2023-005%20Security%20Advisory%20Concerning%20Bypassing%20Secure%20Boot%20and%20Flash%20Encryption%20Using%20EMFI%20EN.html)、
  ESP32-C3 と C6 の AR2023-007）。固めることで「USB ケーブルで数分」が「持ち去った基板への研究室レベルの
  攻撃」に変わりますが、盗まれた機器が安全になるわけではありません。
- **鍵が人目に触れたとき。** 機器のフラッシュ暗号化の鍵を持つ人がその機器を手に入れると、フラッシュの
  チップを直接読んで復号できます（PSK もペアリング情報も）。この鍵は eFuse にあるので、あとから変えられません。
  人目に触れたなら、その機器は固めていないものとして扱ってください。キーと同じように手元で管理するか、
  別の基板に替えます。

### フラッシュ暗号化の鍵を誰が作るか

- **自分のパソコンで作る（この手順）。** その機器の鍵で暗号化したイメージを書き込めば、あとから USB / UART で
  更新できます。鍵はオフラインで保管します。鍵と、機器のフラッシュのチップの両方を手に入れた人は中身を
  復号できます。**機器ごとに別の鍵**にしてください。
- **チップが最初の起動で作る**（このページでは手順を書いていません）。鍵は誰にも分かりませんが、KeyRelay には
  無線での更新（OTA）がないので、ファームは二度と更新できなくなります。

## 全体の流れ

1. 道具と、固めたビルドのイメージ（署名なし）を用意する
2. パソコンで鍵を作る
3. チップを確かめて消去する
4. フラッシュ暗号化の eFuse を書く
5. セキュアブートの eFuse を書く
6. イメージを署名・暗号化して書き込む
7. 最初の起動。ファームが自分でセキュアダウンロードモードを有効にする。すべて確かめる

順番が大事です。

- **フラッシュ暗号化を先、セキュアブートを後に。** セキュアブートは読み出し禁止の eFuse を書き込み禁止に
  するので、先にやると、あとからフラッシュ暗号化の鍵を読み出し禁止にできなくなります。
- **eFuse の作業はすべて、最初の起動より前に。** 最初の起動でファームがセキュアダウンロードモードを
  有効にし、それ以降は `espefuse` で何も書けなくなります。

固めるのは、机の上に出ている机上ユニットからがおすすめです。ペアリング情報は空から始まる（暗号化された
NVS に移るため）ので、2台は自分でペアリングし直します。

## 1. 道具とイメージ

esptool v5 を仮想環境に入れます（以下のコマンドは、その `esptool`・`espefuse`・`espsecure` を使います）。

```sh
python3 -m venv ~/keyrelay-venv
~/keyrelay-venv/bin/pip install esptool
source ~/keyrelay-venv/bin/activate
esptool version
```

固めたイメージは、ESP-IDF v5.4 でリポジトリの一番上から次のようにビルドします（机上ユニットの例。
あとでケースを開けずに更新したいなら、先に `firmware/desk_unit/main/main.c` の `YD_UART_DOWNLOAD_CMD` を
`1` にします）。

```sh
idf.py -C firmware/desk_unit -B firmware/desk_unit/build-hardened \
  -D SDKCONFIG=firmware/desk_unit/build-hardened/sdkconfig \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;../hardening/sdkconfig.hardened" build
```

[`firmware/hardening/sdkconfig.hardened`](../../firmware/hardening/sdkconfig.hardened) と
[`partitions_hardened.csv`](../../firmware/hardening/partitions_hardened.csv) を使います。セキュアブートの
ブートローダーは約 44KB あるので、パーティション表は `0x10000` に移しています。ビルドの最後に
「App built but not signed」と出ますが、署名は手順6で行うので、それで正常です。使うファイルは次の3つです。

- `build-hardened/bootloader/bootloader.bin`
- `build-hardened/partition_table/partition-table.bin`
- `build-hardened/keyrelay_desk_unit.bin`（レシーバーは `keyrelay_receiver.bin`）

## 2. 鍵を作る

鍵はすべてリポジトリの外に置き、オフラインのバックアップ（パスワードマネージャーなど）も取ってください。
下のファイル名は、間違って作業ツリーに置いても `.gitignore` で弾かれる名前にしてあります。鍵ファイルの
中身（や、その base64）は、チャット・Issue・問い合わせ・AI アシスタントに貼り付けないでください。どこかに
貼った鍵は、人目に触れたものとして扱います。

```sh
mkdir -p ~/keyrelay-secrets && cd ~/keyrelay-secrets
# セキュアブートの署名鍵: 2台で共通でかまいません
espsecure generate-signing-key --version 2 --scheme rsa3072 secure-boot-key.pem
espsecure digest-sbv2-public-key --keyfile secure-boot-key.pem --output secure-boot-digest.bin
# フラッシュ暗号化の鍵: 機器ごとに1つ（32バイト、AES-128-XTS）
espsecure generate-flash-encryption-key flash-encryption-key-desk.bin
espsecure generate-flash-encryption-key flash-encryption-key-receiver.bin
```

`secure-boot-key.pem` か、その機器のフラッシュ暗号化の鍵をなくすと、その機器は二度と更新できません。
ペアリング用の鍵 `keyrelay-key.bin` は、今までと同じファイルです（2台共通）。

## 3. つないで、確かめて、消去する

- **机上ユニット:** XIAO 自身の USB-C を、**B** を押しながらつなぎます（組み立て前、またはケースを開けて
  プラグ基板を抜いた状態）。ポートは USB JTAG/serial のもの（VID `0x303a`）です。
- **レシーバー:** COM / UART 側です。

```sh
espefuse --chip esp32s3 -p ポート summary
```

先に進む前に、次を確かめます: `SPI_BOOT_CRYPT_CNT` が `Disable`、`SECURE_BOOT_EN` が `False`、
`ENABLE_SECURITY_DOWNLOAD` が `False`、`KEY_PURPOSE_0` と `KEY_PURPOSE_1` が `USER`（空き）。
違っていたら、そのチップは新品ではないので中止します。

```sh
esptool --chip esp32s3 -p ポート erase-flash
```

## 4. フラッシュ暗号化の eFuse（元に戻せません）

`espefuse` はコマンドごとに `BURN` と打つよう求めます。鍵は、*いま作業している機器の*ものを使います。

```sh
espefuse --chip esp32s3 -p ポート burn-key BLOCK_KEY0 ~/keyrelay-secrets/flash-encryption-key-desk.bin XTS_AES_128_KEY
espefuse --chip esp32s3 -p ポート burn-efuse SPI_BOOT_CRYPT_CNT 7
espefuse --chip esp32s3 -p ポート burn-efuse DIS_DOWNLOAD_ICACHE 1 DIS_DOWNLOAD_DCACHE 1 HARD_DIS_JTAG 1 \
  DIS_DIRECT_BOOT 1 DIS_USB_JTAG 1 DIS_DOWNLOAD_MANUAL_ENCRYPT 1
espefuse --chip esp32s3 -p ポート write-protect-efuse DIS_ICACHE
```

`burn-key` は鍵のブロックを読み出し禁止にします。`summary` では `BLOCK_KEY0` が読めなくなり、
`KEY_PURPOSE_0 = XTS_AES_128_KEY` になります。`DIS_USB_JTAG` が止めるのは USB 経由の JTAG だけです。
XIAO の USB は、キー用の USB ホストとしても、書き込み用としても使えます。

## 5. セキュアブートの eFuse（元に戻せません）

```sh
espefuse --chip esp32s3 -p ポート burn-key BLOCK_KEY1 ~/keyrelay-secrets/secure-boot-digest.bin SECURE_BOOT_DIGEST0
espefuse --chip esp32s3 -p ポート burn-efuse SECURE_BOOT_EN 1 SOFT_DIS_JTAG 7
espefuse --chip esp32s3 -p ポート write-protect-efuse RD_DIS
```

使わない2つの署名鍵の枠は、無効にしなくてかまいません。最初の起動でファームが自分で無効にします
（`Unused SECURE_BOOT_DIGEST1 should be revoked. Fixing..`）。それ以降、その機器が受け付ける鍵は
`secure-boot-key.pem` だけになるので、バックアップは大切に保管してください。`SECURE_BOOT_AGGRESSIVE_REVOKE` は
**書かないでください**。鍵が1つしかないと、検証に失敗したイメージ1つでその鍵が無効になり、二度と
起動しなくなります。

もう一度 `summary` を実行して、出力を残しておきます: `SPI_BOOT_CRYPT_CNT = Enable (0b111)`、
`SECURE_BOOT_EN = True`、`KEY_PURPOSE_1 = SECURE_BOOT_DIGEST0`。

## 6. 署名・暗号化して書き込む

[`firmware/hardening/make_images.sh`](../../firmware/hardening/make_images.sh) が、ブートローダーとアプリへの署名、
ペアリング用の鍵の埋め草と暗号化、各イメージのアドレスに合わせた暗号化をまとめて行い、書き込みのコマンドを
表示します。

```sh
B=firmware/desk_unit/build-hardened
firmware/hardening/make_images.sh $B/bootloader/bootloader.bin $B/keyrelay_desk_unit.bin \
  $B/partition_table/partition-table.bin ~/keyrelay-secrets/secure-boot-key.pem \
  ~/keyrelay-secrets/flash-encryption-key-desk.bin keyrelay-key.bin ~/keyrelay-secrets/images-desk
```

「Signature block 0 is valid」の2行に出る公開鍵のダイジェストが、`secure-boot-digest.bin` と同じかを
確かめます（`xxd -p secure-boot-digest.bin`）。そのあと、表示された「First flash」のコマンドを実行します。
`--after no-reset` を付けると書き込みのあとも起動しないので、最初の起動の前にログを開けます。stub を使わないので、
書き込みには1分ちょっとかかります（約 80kbit/s）。

```sh
esptool --chip esp32s3 -p ポート --no-stub --after no-reset write-flash --flash-mode keep --flash-freq keep --flash-size keep --force \
  0x0 ~/keyrelay-secrets/images-desk/bootloader-enc.bin 0x10000 ~/keyrelay-secrets/images-desk/partition-table-enc.bin \
  0x20000 ~/keyrelay-secrets/images-desk/app-enc.bin 0x190000 ~/keyrelay-secrets/images-desk/ydkey-enc.bin
```

- `--flash-mode keep --flash-freq keep --flash-size keep`: 暗号化・署名済みのブートローダーのヘッダーを
  esptool に書き換えさせないため。
- `--force`: セキュアブートが有効なチップのブートローダーの領域には、これがないと esptool が書き込みません。
  ファイルごとに `Security features enabled, so not changing any flash settings.` と出ますが、それで正常です。
- `nvs_key` パーティション（`0x18000`）は、わざと消去したままにします。NVS の鍵はファームが最初の起動で作ります。

この時点ならまだセキュアダウンロードモードになっていないので、間違えても書き直せます。

## 7. 最初の起動と確認

ログを開いてから（`tio -b 115200 ポート`。机上ユニットは CP2102N のコンソール、レシーバーは COM 側）、
リセットボタンを押します。固めたレシーバーでは、次の順に出ました。

- ROM: `secure boot verification succeeded`
- ブートローダー: `Verifying with RSA-PSS...`、`Signature verified successfully!`、
  `secure boot v2 is already enabled, continuing..`、`flash encryption is enabled (0 plaintext flashes left)`
- アプリ: `Flash encryption mode is RELEASE`、`Unused SECURE_BOOT_DIGEST1 should be revoked. Fixing..`
  （`DIGEST2` も）、`Generating NVS encr-keys...`、`NVS partition "nvs" is encrypted.`、`Key loaded (0x190000)`、
  そのあといつものペアリングと `▶ BLE link up`
- `failed to load RF calibration data` が一度だけ出ます。NVS が空から始まるためで、問題ありません。

セキュアダウンロードモードも、この起動の間にファームが有効にします（専用のログは出ません）。

パソコンからも確かめます。

```sh
esptool --chip esp32s3 -p ポート --no-stub get-security-info
```

`Chip type: ESP32-S3 in Secure Download Mode`、`Secure Boot: Enabled`（Key1 と Key2 は Revoked）、
`Flash Encryption: Enabled`、`SPI Boot Crypt Count (SPI_BOOT_CRYPT_CNT): 0x7`、UART ダウンロードモードの
キャッシュが2つとも Disabled、`JTAG: Permanently Disabled` と出れば成功です。続けて、ペアリング用の鍵を
読み出してみます。これは `The 'read-flash' command is not available in Secure Download Mode.` で
**失敗しなければいけません**。

```sh
esptool --chip esp32s3 -p ポート --no-stub read-flash 0x190000 0x1000 should-fail.bin
```

最後に、[BUILD.md](BUILD.md) の6章の動作確認（FIDO・`gpg --card-status`・OTP）をもう一度行います。

予備の基板では、署名のないファームが拒否されることも確かめられます。*署名していない*アプリを
`espsecure encrypt-flash-data --aes-xts --keyfile … --address 0x20000` で暗号化して `0x20000` に書くと、
ブートローダーが `Secure Boot V2 verification failed.` で止まるはずです。確かめたら、署名したイメージを
書き直してください。

## 固めた機器を更新する

手順1と同じようにビルドし、同じ鍵で `make_images.sh` を実行して、アプリだけを書きます（ペアリング用の鍵と
BLE のペアリング情報は残ります）。

```sh
esptool --chip esp32s3 -p ポート --no-stub write-flash --flash-mode keep --flash-freq keep --flash-size keep \
  0x20000 ~/keyrelay-secrets/images-desk/app-enc.bin
```

esptool が安全のための確認で止まったら、アドレスを確かめてから `--force` を付けます。

ペアリング用の鍵（PSK）を変えるときは、新しい `keyrelay-key.bin` で `make_images.sh` を実行し、
`ydkey-enc.bin` を同じように `0x190000` に書いてから、BLE のペアリング情報を消します（BOOT を3秒）。
これを全部の機器で行います。

ケースを閉じたままの机上ユニットは、シリアルコンソールのダウンロードコマンド（`YD_UART_DOWNLOAD_CMD 1`、
BUILD.md の9章）を使い、`-p ポート` のあとに `--before no-reset` を足します。固めた機器では、この方法の危険は
前よりずっと小さくなります。セキュアダウンロードモードでは、パソコンのソフトはフラッシュに書けても読めず、
書いたものも、あなたの鍵で署名・暗号化していなければ起動しないからです。

## うまくいかないとき

- **手順6のあと起動しない。** 各イメージが、その機器の鍵で、それぞれのアドレス用に暗号化されているかを
  確かめます。`espsecure decrypt-flash-data --aes-xts --keyfile … --address 0x190000 --output check.bin
  ydkey-enc.bin` で `YDK1…` に戻れば正しいです。そのあと書き直します。
- **鍵をなくした。** 署名鍵か、その機器のフラッシュ暗号化の鍵がないと、その機器は更新できません。
  いまのファームのまま動き続けます。
- **ここに書いたことは、どれも元に戻せません。** 固めるのをやめたい機器は、取り替えるしかありません。

机上ユニットを固めた人や、別の基板で試した人は、うまくいったこと（いかなかったこと）を Issue で
教えてください。
