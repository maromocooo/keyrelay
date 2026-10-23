# Hardening a KeyRelay unit (draft, untested)

[日本語](ja/HARDENING.md)

> **Status: draft. Nothing on this page has been tried on KeyRelay hardware yet.** Secure boot and
> flash encryption burn eFuses on the ESP32-S3. **That cannot be undone.** A mistake can leave a board
> that never boots again, or one you can no longer update. Try everything on a spare board first, and
> follow Espressif's official guides, which take precedence over this page:
> [Secure Boot v2](https://docs.espressif.com/projects/esp-idf/en/v5.4/esp32s3/security/secure-boot-v2.html),
> [Flash Encryption](https://docs.espressif.com/projects/esp-idf/en/v5.4/esp32s3/security/flash-encryption.html),
> [NVS Encryption](https://docs.espressif.com/projects/esp-idf/en/v5.4/esp32s3/api-reference/storage/nvs_encryption.html),
> [Security overview](https://docs.espressif.com/projects/esp-idf/en/v5.4/esp32s3/security/security.html).

## What it protects and what it costs

Without hardening, anyone holding a unit can read the pairing key (PSK) and the BLE bonds from flash,
or flash modified firmware that logs PINs (see [SECURITY.md](../SECURITY.md)).

| Feature | Protects | Cost |
|---|---|---|
| Flash encryption | PSK (`ydkey`), app, bonds (together with NVS encryption) against flash readout | Plain `esptool.py write_flash` no longer works for updates |
| Secure boot v2 | Only firmware signed with your key runs | A signing key to keep safe. Losing it means no more updates |
| NVS encryption | BLE bonds and the receiver's saved desk unit | Extra key partition or eFuse key |
| Secure / disabled ROM download mode | Stops flash readout and arbitrary writes over UART/USB | Recovery over UART becomes limited or impossible |

Keep a normal **development build** for experimenting, and a separate **hardened build** for units in
daily use.

## KeyRelay-specific points

### 1. The partition table has to move

A bootloader with secure boot and flash encryption is larger and no longer fits below `0x8000`. Move the
partition table (`CONFIG_PARTITION_TABLE_OFFSET`, e.g. `0x10000`) and shift the partitions after it.
Here is an example that keeps `ydkey` at `0x190000`, so the key-flashing commands stay the same
(**untested**):

```
# Name,   Type, SubType,  Offset,   Size,     Flags
nvs,      data, nvs,      0x11000,  0x6000
phy_init, data, phy,      0x17000,  0x1000
nvs_key,  data, nvs_keys, 0x18000,  0x1000,   encrypted
factory,  app,  factory,  0x20000,  0x170000
ydkey,    data, 0x40,     0x190000, 0x1000,   encrypted
```

`ydkey` is already marked `encrypted` in the repository, so flash encryption covers it.
The app partition shrinks slightly (1.47 MB). The current images are well below that.

### 2. Configuration (both projects, `idf.py menuconfig` → Security features)

- `CONFIG_SECURE_BOOT` with `CONFIG_SECURE_BOOT_V2_ENABLED`. Keep
  `CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES` on and point `CONFIG_SECURE_BOOT_SIGNING_KEY` to a key
  generated with
  `espsecure.py generate_signing_key --version 2 --scheme rsa3072 secure_boot_signing_key.pem`.
  Keep the key outside the repository (`*.pem` is in `.gitignore`) and back it up offline.
- `CONFIG_SECURE_FLASH_ENC_ENABLED`. Use `CONFIG_SECURE_FLASH_ENCRYPTION_MODE_DEVELOPMENT` on the spare
  board, and `CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE` for real use.
- UART ROM download mode: `CONFIG_SECURE_ENABLE_SECURE_ROM_DL_MODE` (Secure Download Mode) or
  `CONFIG_SECURE_DISABLE_ROM_DL_MODE`.
- `CONFIG_NVS_ENCRYPTION` with `CONFIG_NVS_SEC_KEY_PROTECT_USING_FLASH_ENC`, which uses the `nvs_key`
  partition above. It has to be erased before the first start. Alternatively, use
  `CONFIG_NVS_SEC_KEY_PROTECT_USING_HMAC` with an eFuse key.
- Keep `YD_UART_DOWNLOAD_CMD 0` in the desk unit. A hardened unit must not offer a shortcut into the
  bootloader.
- Consider building the receiver with `CONFIG_YD_OTP_KEYBOARD=n` if you do not use OTPs.

### 3. First flash

Flash the merged image and the key file in plaintext, exactly as in [BUILD.md](BUILD.md). On the
**first boot**, the bootloader burns the eFuses and encrypts, in place, the bootloader, the app and every
partition marked `encrypted` (`ydkey`, `nvs_key`). This can take up to a minute. **Do not remove power
during that first boot.** Check the log for flash encryption and secure boot being enabled, and check
`espefuse.py summary`.

### 4. Updates afterwards

- **Development mode:** encrypted re-flashing with `idf.py encrypted-app-flash` or
  `esptool.py --encrypt` still works, but the number of times plaintext flashing can be re-enabled is
  limited by eFuses.
- **Release mode:** serial writes are no longer encrypted for you, and readout is blocked. KeyRelay has no
  OTA update yet. Treat a Release-mode unit as having fixed firmware unless you plan an encrypted or OTA
  update path first.

If you try this, please share what worked (and what did not) in an issue, so this page can stop being a
draft.
