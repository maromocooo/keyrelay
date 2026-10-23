# Hardening a KeyRelay unit

[日本語](ja/HARDENING.md)

> **Status: verified on a computer, not yet on a unit.** The hardened build configuration compiles for both
> units (also in CI), the signing and encryption script was run with test keys, and the eFuse sequence
> below was rehearsed on a virtual ESP32-S3 (`espefuse --virt`). Nobody has run it on real KeyRelay
> hardware yet.
>
> Every eFuse step is **permanent**. A mistake can leave a board that never boots again, or one you can
> no longer update. **Do the whole procedure on a spare board first.** Espressif's documentation takes
> precedence over this page:
> [Security Features Enablement Workflows](https://docs.espressif.com/projects/esp-idf/en/v5.4.4/esp32s3/security/security-features-enablement-workflows.html),
> [Flash Encryption](https://docs.espressif.com/projects/esp-idf/en/v5.4/esp32s3/security/flash-encryption.html),
> [Secure Boot v2](https://docs.espressif.com/projects/esp-idf/en/v5.4/esp32s3/security/secure-boot-v2.html),
> [NVS Encryption](https://docs.espressif.com/projects/esp-idf/en/v5.4/esp32s3/api-reference/storage/nvs_encryption.html).

## What it protects and what it costs

Without hardening, anyone holding a unit for a few minutes can read the pairing key (PSK) and the BLE
bonds from flash, then pose as either unit, or flash firmware that logs PINs (see
[SECURITY.md](../SECURITY.md)). After this procedure:

| Feature | Protects | Cost |
|---|---|---|
| Flash encryption (release mode) | Flash readout gives only ciphertext: PSK, app, NVS keys | Updates must be encrypted with the unit's key (`make_images.sh`) |
| Secure boot v2 | Only firmware signed with your key runs | Keep the signing key safe. Losing it means no more updates |
| NVS encryption | BLE bonds and the receiver's saved desk unit | None in practice (keys are created on the first boot) |
| Secure download mode | The USB/UART ROM loader can no longer read flash or run code in RAM | `esptool` needs `--no-stub`; no readout for debugging |
| JTAG disabled | No debugger access | No debugger access |

Flash encryption in *development* mode is **not** enough: its ROM download mode can load code into RAM,
and that code can read the decrypted flash. This guide therefore uses release mode with secure download
mode.

### Who generates the flash encryption key

- **On your computer (this guide).** You can still update the unit over USB/UART, by flashing images you
  encrypt with its key. Keep the key offline. Anyone holding both the key and the unit's flash chip can
  decrypt it. Use **one key per unit**.
- **By the chip on its first boot** (not described step by step here). Nobody knows the key, but the
  firmware can then never be updated, because KeyRelay has no OTA update.

## Overview

1. Get the tools and the hardened (unsigned) images.
2. Create the keys on your computer.
3. Check and erase the chip.
4. Burn the flash encryption eFuses.
5. Burn the secure boot eFuses.
6. Sign and encrypt the images, flash them.
7. First boot: the firmware turns on secure download mode itself. Check everything.

The order matters:

- **Flash encryption before secure boot.** Secure boot write-protects the read-protection eFuse, which
  would make it impossible to read-protect the flash encryption key afterwards.
- **All eFuse steps before the first boot.** On its first boot the firmware enables secure download
  mode, and from then on `espefuse` cannot burn anything.

Harden the desk unit first: it is the one on the desk. Pairing data starts empty (it now lives in
encrypted NVS), so the two units pair again by themselves.

## 1. Tools and images

Use esptool v5 in a virtual environment (commands below use its `esptool`, `espefuse` and `espsecure`):

```sh
python3 -m venv ~/keyrelay-venv
~/keyrelay-venv/bin/pip install esptool
source ~/keyrelay-venv/bin/activate
esptool version
```

Build the hardened images with ESP-IDF v5.4 from the repository root (the desk unit as an example; for
case-closed updates later, set `YD_UART_DOWNLOAD_CMD 1` in `firmware/desk_unit/main/main.c` first):

```sh
idf.py -C firmware/desk_unit -B firmware/desk_unit/build-hardened \
  -D SDKCONFIG=firmware/desk_unit/build-hardened/sdkconfig \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;../hardening/sdkconfig.hardened" build
```

This uses [`firmware/hardening/sdkconfig.hardened`](../firmware/hardening/sdkconfig.hardened) and
[`partitions_hardened.csv`](../firmware/hardening/partitions_hardened.csv) (partition table at
`0x10000`, because the secure boot bootloader is about 44 KB). The build prints "App built but not
signed": that is expected, signing happens in step 6. The three files you need are:

- `build-hardened/bootloader/bootloader.bin`
- `build-hardened/partition_table/partition-table.bin`
- `build-hardened/keyrelay_desk_unit.bin` (receiver: `keyrelay_receiver.bin`)

## 2. Keys

Keep all keys outside the repository, and make an offline backup (for example in a password manager).
The names below match `.gitignore` patterns, in case one ends up in the working tree by mistake.

```sh
mkdir -p ~/keyrelay-secrets && cd ~/keyrelay-secrets
# Secure boot signing key: one for both units is fine
espsecure generate-signing-key --version 2 --scheme rsa3072 secure-boot-key.pem
espsecure digest-sbv2-public-key --keyfile secure-boot-key.pem --output secure-boot-digest.bin
# Flash encryption key: one per unit (32 bytes, AES-128-XTS)
espsecure generate-flash-encryption-key flash-encryption-key-desk.bin
espsecure generate-flash-encryption-key flash-encryption-key-receiver.bin
```

Losing `secure-boot-key.pem` or a unit's flash encryption key means that unit can never be updated
again. The pairing key `keyrelay-key.bin` is the same file as before (both units).

## 3. Connect, check, erase

- **Desk unit:** connect the XIAO's own USB-C (before assembly, or with the case open and the plug
  breakout removed), holding **B** while plugging in. The port is the USB JTAG/serial one (VID `0x303a`).
- **Receiver:** the COM/UART port.

```sh
espefuse --chip esp32s3 -p PORT summary
```

Check before going on: `SPI_BOOT_CRYPT_CNT` is `Disable`, `SECURE_BOOT_EN` is `False`,
`ENABLE_SECURITY_DOWNLOAD` is `False`, and `KEY_PURPOSE_0` and `KEY_PURPOSE_1` are `USER` (empty). If not,
stop: the chip is not fresh.

```sh
esptool --chip esp32s3 -p PORT erase-flash
```

## 4. Flash encryption eFuses (permanent)

`espefuse` asks you to type `BURN` for each command. Use the key of *this* unit.

```sh
espefuse --chip esp32s3 -p PORT burn-key BLOCK_KEY0 ~/keyrelay-secrets/flash-encryption-key-desk.bin XTS_AES_128_KEY
espefuse --chip esp32s3 -p PORT burn-efuse SPI_BOOT_CRYPT_CNT 7
espefuse --chip esp32s3 -p PORT burn-efuse DIS_DOWNLOAD_ICACHE 1 DIS_DOWNLOAD_DCACHE 1 HARD_DIS_JTAG 1 \
  DIS_DIRECT_BOOT 1 DIS_USB_JTAG 1 DIS_DOWNLOAD_MANUAL_ENCRYPT 1
espefuse --chip esp32s3 -p PORT write-protect-efuse DIS_ICACHE
```

`burn-key` read-protects the key block: `summary` then shows `BLOCK_KEY0` as unreadable and
`KEY_PURPOSE_0 = XTS_AES_128_KEY`. `DIS_USB_JTAG` only disables JTAG over USB; the XIAO's USB port keeps
working as the USB host for the security key and for flashing.

## 5. Secure boot eFuses (permanent)

```sh
espefuse --chip esp32s3 -p PORT burn-key BLOCK_KEY1 ~/keyrelay-secrets/secure-boot-digest.bin SECURE_BOOT_DIGEST0
espefuse --chip esp32s3 -p PORT burn-efuse SECURE_BOOT_EN 1 SOFT_DIS_JTAG 7
espefuse --chip esp32s3 -p PORT write-protect-efuse RD_DIS
```

Optional: if you will never add a second signing key, revoke the two unused slots
(`burn-efuse SECURE_BOOT_KEY_REVOKE1 1 SECURE_BOOT_KEY_REVOKE2 1`). Do **not** burn
`SECURE_BOOT_AGGRESSIVE_REVOKE`: with a single key, one image that fails verification would revoke that
key and the unit could never boot again.

Run `summary` once more and keep its output: `SPI_BOOT_CRYPT_CNT = Enable (0b111)`,
`SECURE_BOOT_EN = True`, `KEY_PURPOSE_1 = SECURE_BOOT_DIGEST0`.

## 6. Sign, encrypt, flash

[`firmware/hardening/make_images.sh`](../firmware/hardening/make_images.sh) signs the bootloader and the
app, pads and encrypts the pairing key, encrypts everything at its offset, and prints the flash commands:

```sh
B=firmware/desk_unit/build-hardened
firmware/hardening/make_images.sh $B/bootloader/bootloader.bin $B/keyrelay_desk_unit.bin \
  $B/partition_table/partition-table.bin ~/keyrelay-secrets/secure-boot-key.pem \
  ~/keyrelay-secrets/flash-encryption-key-desk.bin keyrelay-key.bin ~/keyrelay-secrets/images-desk
```

Check that both "Signature block 0 is valid" lines show the same public key digest as
`secure-boot-digest.bin` (`xxd -p secure-boot-digest.bin`). Then run the printed "First flash" command:

```sh
esptool --chip esp32s3 -p PORT --no-stub write-flash --flash-mode keep --flash-freq keep --flash-size keep --force \
  0x0 ~/keyrelay-secrets/images-desk/bootloader-enc.bin 0x10000 ~/keyrelay-secrets/images-desk/partition-table-enc.bin \
  0x20000 ~/keyrelay-secrets/images-desk/app-enc.bin 0x190000 ~/keyrelay-secrets/images-desk/ydkey-enc.bin
```

- `--flash-mode keep --flash-freq keep --flash-size keep`: esptool must not touch the bootloader header,
  which is encrypted and signed.
- `--force`: esptool refuses to write the bootloader area of a chip with secure boot otherwise.
- The `nvs_key` partition (`0x18000`) stays erased on purpose: the firmware creates the NVS keys on the
  first boot.

If something is wrong, you can still flash again at this point: secure download mode is not on yet.

## 7. First boot and checks

Reset the unit and watch its log (desk unit: the CP2102N console; receiver: the COM port), 115200 baud.
Expected:

- bootloader: `secure boot v2 is already enabled, continuing..`, `Verifying image signature...`,
  `Flash encryption mode is RELEASE`, `Enabling Security download mode...`
- KeyRelay: `NVS partition "nvs" is encrypted.`, `Key loaded (0x190000)`, then the usual pairing and
  `▶ BLE link up`

Then check from the computer:

```sh
esptool --chip esp32s3 -p PORT --no-stub get-security-info
```

It must report secure boot and flash encryption enabled and secure download mode on. Then try to read the
pairing key back; this **must fail**:

```sh
esptool --chip esp32s3 -p PORT --no-stub read-flash 0x190000 0x1000 should-fail.bin
```

Finally repeat the functional checks of [BUILD.md](BUILD.md) section 6 (FIDO, `gpg --card-status`, OTP).

On the spare board you can also check that unsigned firmware is refused: encrypt the *unsigned* app with
`espsecure encrypt-flash-data --aes-xts --keyfile … --address 0x20000`, flash it at `0x20000`, and the
bootloader must stop with `Secure Boot V2 verification failed.`. Flash the signed image again afterwards.

## Updating a hardened unit

Build as in step 1, run `make_images.sh` with the same keys, and flash only the app (the pairing key and
the BLE pairing stay):

```sh
esptool --chip esp32s3 -p PORT --no-stub write-flash --flash-mode keep --flash-freq keep --flash-size keep \
  0x20000 ~/keyrelay-secrets/images-desk/app-enc.bin
```

If esptool stops with a safety check, verify the offset and add `--force`.

For the desk unit with its case closed, use the serial-console download command (needs
`YD_UART_DOWNLOAD_CMD 1`, BUILD.md section 9) and add `--before no-reset` after `-p PORT`. On a hardened
unit this path is much less risky than before: in secure download mode a program on the computer can
write flash but not read it, and anything it writes must be signed and encrypted with your keys to boot.

## If something goes wrong

- **It does not boot after step 6.** Check that each image was encrypted for its own offset with this
  unit's key: `espsecure decrypt-flash-data --aes-xts --keyfile … --address 0x190000 --output check.bin
  ydkey-enc.bin` must give back `YDK1…`. Then flash again.
- **Lost key.** Without the signing key or the unit's flash encryption key, the unit cannot be updated.
  It keeps running the firmware it has.
- **Nothing here can be undone.** A unit you do not want hardened anymore can only be replaced.

If you try this, please report what worked (and what did not) in an issue, so this page can say
"verified on hardware".
