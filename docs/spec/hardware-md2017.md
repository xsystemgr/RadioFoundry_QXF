<!-- SPDX-License-Identifier: MIT -->

# TYT MD-2017 / Retevis RT82: hardware notes

Status: started (2026-10-30), from the vendor image only; nothing has run on a radio yet. The MD-2017 is a member of
the MD-UV380 family ([hardware-mduv380.md](hardware-mduv380.md)); this page lists what the vendor image shows that
differs, and what is still open. Facts marked (image) come from reading TYT's `TYT2017-UVCSV-V4.05.bin`.

## 1. Vendor image

| Item | Value |
|---|---|
| File | `TYT2017-UVCSV-V4.05.bin`, 1276416 bytes, SHA-256 `51944821b12066654101d3c9889472fd111525c2ed114ebeeb67bf1ff1bfc871` |
| Container | OutSecurityBin, model field `MD-9600`, two segments: SPI flash resources at 0x00060000 + 0x5D400, the application at 0x0800C000 + 0xDA400 (image) |
| Encryption | the MD-UV380 key: `stockfw.derive_key` recovers it and its SHA-256 matches `MDUV380_KEY_SHA256` (measured) |
| Application | plain once decrypted, Thumb-2, initial SP 0x20003578, reset handler 0x080E21E9 (image) |

So rfoundry's MD-UV380 file handling (container, key, codec search) is the starting point for this radio; the codec
window of this build is not mapped yet (?).

## 2. RF chips: two AT1846S on one bus

The image drives **two** AT1846S on one bit-banged I2C bus (image):

| Item | Value |
|---|---|
| Bus | SCL **PB6**, SDA **PB7** (GPIOB BSRR set half 0x40020418, reset half 0x4002041A; SDA is switched between output and input through the GPIO init routine for the read). The MD-UV380's single AT1846S is on PA8 / PC9 |
| Write routine | (chip, register, value): chip 0 addresses 0xE2 (7-bit **0x71**), chip 1 addresses 0x5C (7-bit **0x2E**); a mutex around each transfer. Read: the same, 0xE3 / 0x5D |
| Chip 1 (0x2E) | the main transceiver: the TX set-up (0x30 = 0x4046 / 0x7046, TX on) is only ever called for chip 1 |
| Chip 0 (0x71) | receive only: the second receiver. The chip init and the RX set-up (0x30 = 0x4006 / 0x4026, 0x6006 / 0x6026, 0x7006 / 0x7026, by band bits) are called for both chips |

The AT1846S has no AM demodulator (airband-and-broadcast.md 2), so this radio has no air band AM.

## 3. Open questions (next steps)

| # | Question | How |
|---|---|---|
| R1 | How the two receivers' audio reaches the speaker (a mux GPIO, a mixer, both at once) | the image's audio path set-up next to the RX routine's callers |
| R2 | Which receiver feeds the HR_C6000 for DMR, and whether the second can | the HR_C6000 input set-up; the DMR entry's chip arguments |
| R3 | Each chip's LNA, filter and band select pins (the second chip's VHF / UHF switch) | GPIO writes in the RX set-up per chip |
| R4 | Display, keys, knobs, LEDs, PA and backlight pins against the MD-UV380 table | the image's GPIO set-up at start |
| R5 | The codec window of V4.05 for rfoundry | `stockfw` search, as for the MD-UV380 builds |
| R6 | Bootloader entry keys and DFU loader (expected the MD-UV380's: PTT and the top side key at power on) | on the radio |

## 4. Sources

The vendor image above, read with `tools/rfoundry` (container, key) and `arm-none-eabi-objdump`. Nothing from other
open firmware for this radio is used (docs/CLEANROOM.md).
