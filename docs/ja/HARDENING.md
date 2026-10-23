# KeyRelay の守りを固める（下書き・未検証）

[English](../HARDENING.md)

> **状態: 下書きです。このページの内容は、まだ KeyRelay の実機で1つも試していません。**
> セキュアブートとフラッシュ暗号化は、ESP32-S3 の eFuse を書き込みます。**元には戻せません。**
> 間違えると、二度と起動しない、または二度と更新できない基板になることがあります。必ず予備の基板で
> 先に試し、Espressif の公式ガイドに従ってください（このページより公式ガイドが優先です）:
> [Secure Boot v2](https://docs.espressif.com/projects/esp-idf/en/v5.4/esp32s3/security/secure-boot-v2.html)、
> [Flash Encryption](https://docs.espressif.com/projects/esp-idf/en/v5.4/esp32s3/security/flash-encryption.html)、
> [NVS Encryption](https://docs.espressif.com/projects/esp-idf/en/v5.4/esp32s3/api-reference/storage/nvs_encryption.html)、
> [セキュリティの概要](https://docs.espressif.com/projects/esp-idf/en/v5.4/esp32s3/security/security.html)。

## 何が守れて、何を失うか

何もしないと、機器を手に入れた人はフラッシュからペアリング用の鍵（PSK）と BLE のペアリング情報を
読み出せます。PIN を記録するように書き換えたファームを書き込むこともできます（[SECURITY.md](SECURITY.md)）。

| 機能 | 守れるもの | 代わりに失うもの |
|---|---|---|
| フラッシュ暗号化 | PSK（`ydkey`）・アプリ・ペアリング情報（NVS 暗号化と組み合わせて）の読み出し | ふつうの `esptool.py write_flash` で更新できなくなる |
| セキュアブート v2 | 自分の鍵で署名したファームしか動かない | 署名鍵の管理。なくすと二度と更新できない |
| NVS 暗号化 | BLE のペアリング情報、レシーバーが覚えた机上ユニット | 鍵用のパーティションか eFuse の鍵 |
| ROM のダウンロードモードを制限・無効化 | UART / USB からの読み出しと勝手な書き込み | UART からの復旧が限られる、またはできなくなる |

いじって遊ぶ用の**開発ビルド**と、普段使う機器用の**固めたビルド**を分けて持つのがおすすめです。

## KeyRelay で気をつけること

### 1. パーティション表をずらす必要がある

セキュアブートとフラッシュ暗号化を入れたブートローダーは大きくなり、`0x8000` の手前に収まらなく
なります。パーティション表の位置（`CONFIG_PARTITION_TABLE_OFFSET`、例: `0x10000`）を後ろにずらし、
後ろのパーティションも合わせてずらします。`ydkey` を `0x190000` のまま残して、鍵の書き込みコマンドを
変えずに済む例です（**未検証**）:

```
# Name,   Type, SubType,  Offset,   Size,     Flags
nvs,      data, nvs,      0x11000,  0x6000
phy_init, data, phy,      0x17000,  0x1000
nvs_key,  data, nvs_keys, 0x18000,  0x1000,   encrypted
factory,  app,  factory,  0x20000,  0x170000
ydkey,    data, 0x40,     0x190000, 0x1000,   encrypted
```

`ydkey` にはリポジトリの時点で `encrypted` を付けてあるので、フラッシュ暗号化の対象になります。
アプリの領域は少し小さくなりますが（1.47MB）、今のイメージは十分に収まります。

### 2. 設定（両方のプロジェクトで `idf.py menuconfig` → Security features）

- `CONFIG_SECURE_BOOT` と `CONFIG_SECURE_BOOT_V2_ENABLED`。
  `CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES` はオンのままにし、`CONFIG_SECURE_BOOT_SIGNING_KEY` に
  次のコマンドで作った鍵を指定します。
  `espsecure.py generate_signing_key --version 2 --scheme rsa3072 secure_boot_signing_key.pem`
  鍵はリポジトリの外に置き（`*.pem` は `.gitignore` に入っています）、オフラインでバックアップします。
- `CONFIG_SECURE_FLASH_ENC_ENABLED`。予備の基板では `CONFIG_SECURE_FLASH_ENCRYPTION_MODE_DEVELOPMENT`、
  普段使いでは `CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE`。
- UART の ROM ダウンロードモード: `CONFIG_SECURE_ENABLE_SECURE_ROM_DL_MODE`（Secure Download Mode）か
  `CONFIG_SECURE_DISABLE_ROM_DL_MODE`。
- `CONFIG_NVS_ENCRYPTION` と `CONFIG_NVS_SEC_KEY_PROTECT_USING_FLASH_ENC`（上の `nvs_key` パーティションを
  使う）。`nvs_key` は最初に起動する前に消去しておく必要があります。eFuse の鍵を使う
  `CONFIG_NVS_SEC_KEY_PROTECT_USING_HMAC` もあります。
- 机上ユニットの `YD_UART_DOWNLOAD_CMD` は `0` のまま。固めた機器に、ブートローダーへの近道を残さない。
- OTP を使わないなら、レシーバーは `CONFIG_YD_OTP_KEYBOARD=n` でビルドすることも検討してください。

### 3. 最初の書き込み

まとめたイメージと鍵ファイルを、[BUILD.md](BUILD.md) とまったく同じように平文で書き込みます。
**最初の起動**で、ブートローダーが eFuse を書き込み、ブートローダー・アプリ・`encrypted` の付いた
パーティション（`ydkey`、`nvs_key`）をその場で暗号化します。最大1分ほどかかります。
**この最初の起動の間は、絶対に電源を抜かないでください。** ログでフラッシュ暗号化とセキュアブートが
有効になったことを確かめ、`espefuse.py summary` でも確認します。

### 4. そのあとの更新

- **Development モード:** `idf.py encrypted-app-flash` や `esptool.py --encrypt` で、暗号化して書き直せます。
  ただし、平文の書き込みを有効に戻せる回数は eFuse で限られています。
- **Release モード:** シリアルから書いても暗号化してくれず、読み出しも止まります。KeyRelay には OTA
  （無線での更新）がまだありません。暗号化済みイメージか OTA での更新方法を先に用意しない限り、
  Release モードの機器のファームは固定と考えてください。

試した人は、うまくいったこと（いかなかったこと）を Issue で教えてください。このページを下書きから
卒業させたいです。
