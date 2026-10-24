# Security

KeyRelay relays a hardware security key over Bluetooth LE. That is a deliberate change to the key's
threat model, so read this before you rely on it.

## Reporting a vulnerability

Please report vulnerabilities privately through GitHub's
**Security → Report a vulnerability** (private vulnerability reporting) on this repository.
Do not open a public issue for security problems. This is a hobby project maintained in spare time;
expect a reply within a couple of weeks.

## What the relay is

```
Mac/PC ──USB── receiver (ESP32-S3) ── BLE ── desk unit (ESP32-S3, USB host) ──USB── security key
```

The receiver presents itself to the computer as a FIDO HID device, a CCID smart-card reader and an
OTP keyboard. Messages are forwarded unchanged to the key in the desk unit and back, with one
exception: the desk unit virtualises CTAPHID channel IDs (it answers the computer's broadcast
`CTAPHID_INIT` itself and rewrites the 4-byte channel ID of each FIDO packet), so channels survive a
re-enumeration of the key. The relay does not parse CTAP commands, APDUs or OTP frames beyond what is
needed to split and reassemble them.

**Private keys never leave the security key.** FIDO credentials, PIV/OpenPGP private keys, OATH seeds
and HMAC challenge-response secrets stay inside the key, exactly as with a directly attached key.
**User presence is still a touch on the key**, which sits in the desk unit.

## How the BLE link is protected

Two independent layers:

1. **BLE LE Secure Connections** (ECDH, "Just Works", bonded). The desk unit only accepts a *new*
   bond while it has no bond yet or during the first 120 s after boot. Just Works cannot stop a
   man-in-the-middle during that pairing moment, which is why layer 2 exists. The receiver only
   remembers a desk unit after the layer-2 handshake succeeds. Both units delete the BLE bond of any
   peer that paired during this connection but did not complete that handshake (whatever the reason,
   including a peer that disconnects by itself), and of any peer whose handshake shows a different PSK.
   A bond that existed before the connection is kept when the handshake only times out or fails
   otherwise: the peer could encrypt with it, so it is the device the bond was made with. The receiver
   never deletes the registered desk unit's bond when encryption fails and does not re-pair with it,
   because a device without the PSK can claim the desk unit's address and fail encryption on purpose.
   The desk unit lets a peer it already has a bond with pair again only within its 120 s window (for
   example a receiver that lost its bond) and refuses outside it. Before deleting the old bond it keeps
   a copy in RAM, and it writes that copy back unless the peer that paired again completes the layer-2
   handshake on that connection, so claiming the receiver's address within the window does not replace
   its bond either. A second re-pairing on the same connection is refused. When the bond store is full,
   no existing bond is evicted to make room. A device without the PSK therefore cannot make the
   receiver stick to it, keep the desk unit's bond for itself, or push the legitimate peer out. If you
   erase the desk unit's bonds, erase the receiver's too (see "Generating the pre-shared key" below).
2. **Application layer with a pre-shared key (PSK) and ephemeral X25519** (link protocol v2), implemented in
   [`firmware/components/yd_link/yd_sec.c`](firmware/components/yd_link/yd_sec.c):
   - The same random 256-bit key is flashed into both units (`YDK1` + 32 bytes at `0x190000`,
     partition `ydkey`). Without a valid key, BLE is never started.
   - The units find each other without a fixed identifier: the desk unit advertises no name, and the GATT
     service and characteristic UUIDs are derived from the PSK (each is the first 16 bytes of
     `HMAC-SHA256(PSK, "keyrelay v2 gatt uuid" ‖ n)`, n = 0, 1, 2, so they have no visible relation to each
     other). A scanner cannot pick out a KeyRelay unit by a well-known UUID or name, and a receiver ignores
     desk units set up with a different PSK. Both units log the first four bytes of the service UUID as
     `Link ID` at boot; they must match. A receiver that sees its registered desk unit, or a desk unit with
     older firmware, advertising something else says so in its log.
   - On every connection, both sides send a 16-byte random nonce and a fresh X25519 public key. The
     X25519 private keys exist only in RAM and are wiped right after the key derivation. Per-connection
     keys come from HKDF-SHA256 with the PSK and the X25519 shared secret as input key material and both
     nonces and both public keys as salt: one key per direction plus an authentication key. Public keys
     that give an all-zero shared secret (low-order points) are rejected. Because the per-connection
     keys depend on secrets that were never stored, recorded traffic stays confidential even if the PSK
     and the BLE bonds are read from a unit later (forward secrecy, as in Noise `NNpsk0` or TLS 1.3
     `psk_dhe_ke`).
   - Mutual authentication: each side sends `HMAC-SHA256(k_auth, "YD-AUTH" ‖ role ‖ nonces)` (truncated
     to 16 bytes) and checks the peer's value in constant time. A wrong value disconnects, and so does a
     handshake that is not finished 5 s after the encrypted BLE link is up. A connection that has not
     completed the handshake 15 s after it was opened (for example a peer that never encrypts or never
     subscribes) is closed too. A peer that keeps reconnecting can still hold the single connection
     slot most of the time: denial of service, like radio jamming.
   - Data: AES-256-GCM per packet, the 5-byte header (type + 32-bit counter) is authenticated as AAD,
     the counter forms the IV. Each packet's counter must be exactly one more than the previous one, so
     replayed, reordered and selectively dropped packets are all detected and end the connection. The
     link is dropped before the counter could wrap.
   - Before encryption, every packet is padded (`0x80`, then zeros) to a multiple of 32 bytes, or to the
     largest size that fits in one packet when that is smaller, so packet sizes do not show the length of
     a PIN (OpenPGP sends PINs unpadded). The keystrokes of one touch are
     sent as a fixed sequence: one packet of the same size every 50 ms for at least 2 seconds, whatever
     was typed, so neither packet sizes nor their timing show how long a typed OTP or static password is.
   - Both units must run the same link protocol version. A unit that meets a different version logs it
     and disconnects; update both units together.

An attacker in radio range **without the PSK** can jam or disconnect the link (denial of service),
but cannot read, inject, replay or selectively drop relayed messages.

## If the PSK leaks

The PSK is a symmetric key shared by all units (the desk unit and each receiver). Realistically it leaks
by reading the flash of any unit (flash encryption is off by default) or from a copy of `keyrelay-key.bin`
left on a computer. Flash readout also gives the BLE bonds (LTKs), so the BLE encryption does not help in
that case.

- **Past traffic stays confidential.** Recordings of earlier connections cannot be decrypted with the PSK
  and the bonds, because the X25519 secrets of those connections were never stored (see above).
- **Posing as the desk unit (the more serious direction).** With the PSK, a fake desk unit can connect to
  your real receiver. The receiver is a USB keyboard to your computer, so the fake unit could make it
  **type keystrokes, with no touch and no PIN**. By default the receiver only types input shaped like a
  Yubico OTP (`CONFIG_YD_OTP_KBD_FILTER_MODHEX`): in one burst of keystrokes, up to 64 of the 16 ModHex
  letters without modifier keys, and an Enter only right after 32 to 64 of them (one Enter per OTP; an OTP
  and Enter can follow another OTP and Enter in the same burst). Anything else blocks the rest of that
  burst. Each report may hold one key, and the receiver types each accepted key as a press
  and a release itself, so holding a key down for the computer's key repeat does not work either. A fake
  desk unit therefore cannot type commands, other characters, or a short
  answer followed by Enter. It can still type ModHex letters, and an Enter after 32 of them, which apps
  with single-letter shortcuts (vim, some web apps) and a dialog's default button react to, as they
  would to an OTP typed at the wrong moment. If you choose "any key" for static passwords, read the
  keystroke-injection item below. If you do not use OTPs, build the receiver with
  `CONFIG_YD_OTP_KEYBOARD=n`: it then has no keyboard at all. A fake desk unit can also answer
  smart-card and FIDO requests itself, but it cannot produce signatures or assertions of your key.
- **Posing as the receiver.** A fake receiver can talk to your key from radio range. Most operations still
  need the PIN and a touch, but not all of them:
  - OATH (TOTP/HOTP) codes can be calculated without a touch or PIN unless you set an OATH password and
    require touch for the credentials (`ykman oath access change`, `ykman oath accounts add --touch`).
  - OTP challenge-response (HMAC-SHA1, used by KeePassXC) answers without a touch unless the slot was
    programmed with `--touch`.
  - Wrong PIN attempts can use up the PIN retry counters and lock PIV/OpenPGP PINs (denial of service).
  - It can also **break the key's configuration without a touch** (denial of service, not theft of
    secrets). Over the smart-card interface, which the desk unit relays without reading APDUs, that
    includes resetting the OATH application, resetting PIV or OpenPGP after using up the PIN and PUK
    retries, and changing the key's settings through the management application (enabled interfaces,
    a configuration lock code). The desk unit does block OTP commands that change the key (programming,
    updating, swapping or deleting slots, access codes, NDEF, device settings): only challenge-response
    and reading the serial number and capabilities are relayed. Slots therefore cannot be configured
    through KeyRelay; plug the key into a computer for that.
- **You may not notice.** A stolen key is missed quickly, a silently copied PSK is not. If you suspect a
  leak, generate a new key file, flash it to all units and erase the BLE bonds.

The real fix is to keep the PSK from being read: see [docs/HARDENING.md](docs/HARDENING.md).
The desk unit sits on the desk, so harden it first.

## If a unit is lost or stolen

A unit in someone else's hands is still a working KeyRelay unit, hardened or not. It does not need to
read anything out of its flash to talk to your other units.

- **A receiver.** It has the PSK and a bond with your desk unit, so from radio range it can use your key
  like your own receiver would (see "Posing as the receiver" above). Erasing the desk unit's bonds (BOOT
  for 3 s) does **not** lock it out: a desk unit without bonds accepts a new pairing, and the lost
  receiver can complete the handshake because it has the PSK.
- **The desk unit.** If the security key was in it, deal with the key first, as with any lost key. The
  desk unit itself has the PSK and a bond with each receiver, so it can pose as the desk unit to them
  (see "Posing as the desk unit" above).

In both cases, generate a new PSK and flash it to every unit you still have, then erase the BLE bonds on
them. Units with the old PSK can no longer find or authenticate the others.

This follows from the design: one PSK is shared by all units, so changing it is the only way to revoke
a unit, and it means reflashing all the others. Per-unit keys (each unit with its own key pair, and the
desk unit with a list of the receivers it accepts, as in Noise `KK`) would make it possible to remove
one receiver without touching the others. That would be a new link protocol version and is not
implemented.

## What KeyRelay does *not* protect against

- **The relay sees what the computer sends in plaintext.** That includes PIV PINs, OpenPGP PINs and
  admin PINs, PIV management keys, OATH codes, typed OTPs and challenge-response inputs/outputs.
  FIDO2 PINs are the exception: CTAP2's PIN/UV auth protocol encrypts them between the computer and the
  key with a key the relay does not know (U2F has no PIN). Both units run code you must trust: only
  flash firmware you built or verified yourself.
- **Keystroke injection.** The receiver is a USB keyboard as far as the computer is concerned (that is
  how OTPs are typed). Modified receiver firmware, or a look-alike receiver someone plugs in, could type
  arbitrary keystrokes, like any malicious USB keyboard. A fake desk unit holding the PSK could make the
  real receiver type, too; the default filter limits that to input shaped like a Yubico OTP (see "If
  the PSK leaks"). If a slot types a static password or OATH-HOTP digits, you have to select "any key"
  (`CONFIG_YD_OTP_KBD_FILTER_NONE`), which removes that limit. If you do not use OTPs, build the receiver
  with `CONFIG_YD_OTP_KEYBOARD=n` (menuconfig → KeyRelay): it then has no keyboard interface at all.
- **A touch approves whatever is pending.** The touch proves that someone is at the desk unit, not that
  the request came from the application you meant. If the computer is compromised, an unexpected
  request can blink the key; do not touch it then. FIDO2/WebAuthn still binds every request to the site
  (RP ID), but PIV, OpenPGP and OTP have no such binding, and many PIV/OpenPGP operations need only a
  PIN by default. Consider the key's touch policies (PIV: `--touch-policy` when generating or importing
  a key; OpenPGP: `ykman openpgp keys set-touch`).
- **Physical access to either unit.** Flash encryption and secure boot are **not** enabled by default,
  so anyone holding a unit for a few minutes can read the PSK and the BLE bond, then pose as either unit
  (see "If the PSK leaks"). Keep the desk unit where you would keep the key itself.
  [docs/HARDENING.md](docs/HARDENING.md) turns on flash encryption (release mode), secure boot v2, NVS
  encryption and secure download mode, with a configuration that builds for both units and a script for
  signing and encrypting. It has been verified on a receiver (not yet on a desk unit), and every eFuse
  step is irreversible: try it on a spare board first. Hardening keeps the PSK from being read out; it
  does not stop a stolen unit from being used (see "If a unit is lost or stolen"), and the ESP32-S3 does
  not resist fault injection on an opened board (see HARDENING.md).
- **Plaintext in the units' memory.** The link layer wipes the buffers it encrypts from and decrypts
  into, but its send queue and the USB and reassembly buffers that carry PINs and APDUs keep their
  contents until they are reused.
  Reading RAM needs a debugger (JTAG, which hardening disables) or a physical attack on the chip.
- **The desk unit's serial console, if you enable updates over it.** With `YD_UART_DOWNLOAD_CMD 1`
  in `firmware/desk_unit/main/main.c` (default `0`), sending the line `keyrelay-download` to the desk
  unit's USB-UART reboots it into the ROM bootloader, so you can update it without opening the case.
  While that USB-C port is connected to a computer, **any program on that computer** can then reflash
  the unit or read its flash, including the PSK. If you enable it, power the desk unit from a plain
  charger in daily use. On a hardened unit (secure download mode) the risk is much smaller: a program on
  the computer can no longer read flash, and anything it writes must be signed and encrypted with your
  keys to boot.
- **The receiver's UART ("COM") port.** On typical ESP32-S3 dev boards this port can reset the chip
  into the bootloader. Leave it unplugged in daily use; only the native USB port needs to be connected.
- **Relay distance.** WebAuthn assumes the authenticator is physically next to the computer. KeyRelay
  extends that to BLE range on purpose. The touch requirement is what keeps a remote attacker from
  using the key without someone at the desk unit.
- **What the radio still shows.** Padding hides exact lengths, but not everything: that the link is in
  use and when, that a touch typed something, the number of packets and the size class of each (for
  example the rough size of a large smart-card response). If typing takes longer than 2 seconds (for
  example with the key's pacing option), the extra time shows. The units advertise no name or fixed UUID,
  but their Bluetooth addresses are fixed, so a unit can be recognised again whenever it is in range, and
  the connection shows which two units belong together.
- **NFC on the key.** The relay does not change the key's NFC interface. A YubiKey 5 NFC left on the
  desk answers any phone held against it, and by default its NFC interface hands out a Yubico OTP.
  If you do not use NFC, turn it off: plug the key directly into a computer and run
  `ykman config nfc --disable-all` (KeyRelay hides the key from ykman).
- **Attestation and organization policies.** The relayed key's attestation is genuine, so services
  cannot tell that it is used over BLE. That is fine for your own accounts. For a key issued by your
  employer, check that relaying it is allowed.
- **Key firmware.** YubiKey 5 series firmware before 5.7 is affected by
  [YSA-2024-03](https://www.yubico.com/support/security-advisories/ysa-2024-03/) ("EUCLEAK"): with the
  key in hand, opened and with expensive equipment, an ECDSA private key can be recovered. The key used
  in development has firmware 5.1.2. The risk is low, but a key that lives on a desk is easier to take
  for a while, and YubiKey firmware cannot be updated.

## Generating the pre-shared key

Generate it on your own computer and never commit it (`*key*.bin` is in `.gitignore`):

```sh
(printf 'YDK1'; head -c 32 /dev/urandom) > keyrelay-key.bin
```

Flash the same file to **all** units (the desk unit and each receiver) at `0x190000`. To rotate the key,
generate a new file and flash it to all units (hardened units: see "Updating a hardened unit" in
[docs/HARDENING.md](docs/HARDENING.md)). The key also determines the BLE service UUIDs, so units with
different keys do not even see each other.

BLE bonds are stored in the `nvs` partition. They are cleared by holding BOOT for 3 s (receiver: while
held; desk unit: on release, and its BOOT button is inside the case), and also by flashing a full merged
image at `0x0`, which overwrites `nvs`. A desk unit without a bond, or within 120 s of booting, accepts
a new bond; only a peer holding the PSK can then complete the KeyRelay handshake. Erasing bonds
therefore does not lock out a unit that still has the PSK (see "If a unit is lost or stolen").
The receiver keeps its registered desk unit's bond even when the desk unit no longer has it (see
"How the BLE link is protected"), so after erasing the desk unit's bonds, erase the receiver's as well;
they then pair again on their own within the desk unit's 120 s window.
