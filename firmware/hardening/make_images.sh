#!/bin/sh
# Sign and encrypt the images of one hardened KeyRelay unit (docs/HARDENING.md).
#
#   make_images.sh <bootloader.bin> <app.bin> <partition-table.bin> <secure-boot-key.pem> \
#                  <flash-encryption-key.bin> <keyrelay-key.bin> <output-dir>
#
# Needs espsecure from esptool v5 (pip install esptool). Offsets match partitions_hardened.csv.
# Writes the encrypted images to <output-dir> and prints the esptool commands to flash them.
set -eu

if [ "$#" -ne 7 ]; then
    sed -n '2,9p' "$0"
    exit 2
fi
BOOT=$1; APP=$2; PART=$3; SBKEY=$4; FEKEY=$5; PSK=$6; OUT=$7

BOOT_OFF=0x0
PART_OFF=0x10000
APP_OFF=0x20000
KEY_OFF=0x190000

if command -v espsecure >/dev/null 2>&1; then
    ESPSECURE=espsecure
elif command -v espsecure.py >/dev/null 2>&1; then
    ESPSECURE=espsecure.py
else
    echo "espsecure not found: pip install esptool" >&2
    exit 1
fi

for f in "$BOOT" "$APP" "$PART" "$SBKEY" "$FEKEY" "$PSK"; do
    [ -f "$f" ] || { echo "missing file: $f" >&2; exit 1; }
done
[ "$(wc -c < "$FEKEY")" -eq 32 ] || { echo "flash encryption key must be 32 bytes (AES-128-XTS)" >&2; exit 1; }
[ "$(wc -c < "$PSK")" -eq 36 ] && [ "$(head -c 4 "$PSK")" = "YDK1" ] || { echo "not a KeyRelay key file: $PSK" >&2; exit 1; }

mkdir -p "$OUT"

echo "== Signing (secure boot v2)"
$ESPSECURE sign-data --version 2 --keyfile "$SBKEY" --output "$OUT/bootloader-signed.bin" "$BOOT"
$ESPSECURE sign-data --version 2 --keyfile "$SBKEY" --output "$OUT/app-signed.bin" "$APP"
$ESPSECURE signature-info-v2 "$OUT/bootloader-signed.bin"
$ESPSECURE signature-info-v2 "$OUT/app-signed.bin"

echo "== Padding the KeyRelay key to one 4 KB flash sector (0xFF)"
cp "$PSK" "$OUT/ydkey-plain.bin"
head -c $((4096 - 36)) /dev/zero | LC_ALL=C tr '\000' '\377' >> "$OUT/ydkey-plain.bin"

echo "== Encrypting (flash encryption, AES-128-XTS). Each image is bound to its offset"
$ESPSECURE encrypt-flash-data --aes-xts --keyfile "$FEKEY" --address $BOOT_OFF --output "$OUT/bootloader-enc.bin" "$OUT/bootloader-signed.bin"
$ESPSECURE encrypt-flash-data --aes-xts --keyfile "$FEKEY" --address $PART_OFF --output "$OUT/partition-table-enc.bin" "$PART"
$ESPSECURE encrypt-flash-data --aes-xts --keyfile "$FEKEY" --address $APP_OFF --output "$OUT/app-enc.bin" "$OUT/app-signed.bin"
$ESPSECURE encrypt-flash-data --aes-xts --keyfile "$FEKEY" --address $KEY_OFF --output "$OUT/ydkey-enc.bin" "$OUT/ydkey-plain.bin"
rm -f "$OUT/ydkey-plain.bin" "$OUT/bootloader-signed.bin" "$OUT/app-signed.bin"

cat <<EOF

== Done. Images in $OUT
First flash (whole unit, see docs/HARDENING.md for the order of eFuse steps):
  esptool --chip esp32s3 -p PORT --no-stub write-flash --flash-mode keep --flash-freq keep --flash-size keep --force \\
    $BOOT_OFF $OUT/bootloader-enc.bin $PART_OFF $OUT/partition-table-enc.bin $APP_OFF $OUT/app-enc.bin $KEY_OFF $OUT/ydkey-enc.bin
Update (app only, keeps the key and the pairing):
  esptool --chip esp32s3 -p PORT --no-stub write-flash --flash-mode keep --flash-freq keep --flash-size keep \\
    $APP_OFF $OUT/app-enc.bin
EOF
