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

### 2.1 Tuning the two receivers

One routine (0x0804602C) retunes the receivers (image):

| Mode | What it does |
|---|---|
| One receiver | only the selected channel's chip: channel A on chip 1 (0x2E), channel B on chip 0 (0x71); which one is a flag in RAM (0x2001C930 bit 7, 1 = A) |
| Both | chip 1 with channel A and chip 0 with channel B, each with its own frequency: **true dual receive**, not alternation |

Before it tunes, when the selected channel changed, it switches two pins by the same A/B bit, always to opposite levels
(GPIO writes through the image's `GPIO_WriteBit`, 0x0803A5CE):

| Selected | PD9 | PD8 |
|---|---|---|
| A (chip 1, 0x2E) | 1 | 0 |
| B (chip 0, 0x71) | 0 | 1 |

### 2.3 The receivers' audio: PD9 for A, PD8 for B

PD9 and PD8 are not a one-of-two switch: the image drives all four combinations. Each connects one receiver's audio
to a shared line (inferred: the line the HR_C6000 and the audio path take, as on the MD-UV380 family), PD9 receiver
A (chip 1), PD8 receiver B (chip 0) (image):

| Routine | What it does |
|---|---|
| unmute (0x08060912, takes the chip) | the chip's audio registers (0x41, 0x44 = 0xFF), then the chip's switch on: an analog channel (the TYT codeplug's type 1) turns on PD9 for A or PD8 for B, the other switch as it was |
| digital (0x0806077A, takes the chip) | a digital channel (type 2): the chip's switch on and the other's off (A: PD9 = 1, PD8 = 0; B: PD8 = 1, PD9 = 0) |
| retune (2.1) | when the selected side changes: its switch on, the other off |

So in FM both receivers can be heard at once (both switches on); in DMR only one receiver feeds the line, since the one
HR_C6000 decodes one signal (inferred from the switch levels; what is heard is checked on the radio).
On the MD-UV380 PD9 is the RX audio mux and PD8 the LCD backlight; on the MD-2017 PD8 is this switch, so the backlight
is on another pin (?: R4). The same routine writes PC4, PC5 and PC6 low (PC6 is the PA band select on the MD-UV380,
PC5 in its TX path).

### 2.2 Front-end supplies per receiver

Each receiver has its own VHF and UHF RX supply. A routine (0x0804531E) reads both channels' frequencies (stored in
BCD: 0x40000000 = 400 MHz, 0x20000000 = 200 MHz) and switches them per band, after a routine that first drives all of
them and the TX supplies (PC4, PC5) low (image):

| Receiver | UHF (>= 400 MHz) | VHF (<= 200 MHz) |
|---|---|---|
| A, chip 1 (0x2E) | **PC9** | **PA5** |
| B, chip 0 (0x71) | **PA8** | **PA2** |

All four band combinations occur (A UHF + B VHF: PC9 and PA2, and so on), so the two receivers can sit in different
bands. On the MD-UV380, PA2 / PA5 are its one receiver's UHF / VHF supplies and PA8 / PC9 are the AT1846S I2C pins.

## 3. Pin map against the MD-UV380

From every GPIO_Init (mode), GPIO_PinAFConfig (alternate function) and read / write call in the image (image).
"Same" means the MD-UV380 column of [hardware-mduv380.md](hardware-mduv380.md) and `boards/uv380_family/pins.h`.

| Pin | MD-2017 | MD-UV380 | |
|---|---|---|---|
| PA0, PA1, PA3, PA4 | analog | volume, battery, mic level, APC DAC | same |
| PA2 / PA5 | out | RX UHF / VHF supply | **receiver B VHF / receiver A VHF** (2.2) |
| PA6, PD2, PD3 | in pull-up / out | key rows | same |
| PA7 | out | power latch | same |
| PA8 / PC9 | out | AT1846S I2C3 SCL / SDA | **receiver B UHF / receiver A UHF supply** |
| PA11, PA12 | AF10 | USB | same |
| PA13 | in pull-up, read | mic power (out) | **differs** (?) |
| PA15 | in | — | new (?) |
| PB0, PB1 | in | (PB1 AT1846S GPIO0, unverified) | (?) |
| PB2 | out, written in the RX / TX switching | (AT1846S GPIO6, unverified) | (?) |
| PB3-PB5 | AF5 | SPI flash | same |
| PB6 / PB7 | out / in-out | — | **AT1846S bus SCL / SDA** (2) |
| PB8, PB9 | out | speaker mute, audio amplifier | same |
| PB10 | in pull-up | — | new input (?) |
| PB11, PE14 | in pull-up | channel encoder | same |
| PB12, PB13-PB15 | out, AF5 | HR_C6000 voice SPI2 | same |
| PC0-PC2 | in | HR_C6000 interrupts | same |
| PC4, PC5 | out | APC enable, TX supply | same |
| PC6 | out | PA band select | same (written low in RX) |
| PC7, PC8 | AF3 TIM8 | CTCSS / beep PWM | same |
| PC13 | out | — | new (?) |
| PD0, PD1, PD4, PD5, PD12, PD14, PD15, PE7-PE10 | AF12 FSMC | LCD bus | same |
| PD6, PD13 | out | LCD CS, LCD reset | same |
| PD8 / PD9 | out, opposite levels | backlight / RX audio mux | **receiver switch B / A** (2.1); the backlight is elsewhere (?) |
| PD10, PD11 | in, read | — | new inputs (a second knob? ?) |
| PE0, PE1 | out | LEDs | same |
| PE2-PE6 | out / in | HR_C6000 control SPI, PWD | same pins (the directions not all decoded yet) |
| PE11, PE12 | in pull-up | PTT, external PTT | same |
| PE13, PE15 | in pull-up | — | new inputs (keys? ?) |

## 4. Open questions (next steps)

| # | Question | How |
|---|---|---|
| R1 | ~~The receivers' audio~~: PD9 A, PD8 B, both on in FM (2.3); confirm on the radio | |
| R2 | ~~DMR input~~: the selected receiver alone, either one (2.3); confirm on the radio | |
| R3 | ~~Each receiver's band supplies~~: done (2.2) | |
| R4 | The pins marked (?) in section 3: the backlight, PA13, PA15, PB0-PB2, PB10, PC13, PD10, PD11, PE13, PE15 | the code reading or writing each one; then on the radio |
| R5 | The codec windows of V4.05 for rfoundry (a profile in `stockfw.PROFILES`) | the first code word 0x4FF0E92D is a common prologue, so it doesn't locate the codec alone. The codec's tables are the MD-UV380 library's, moved: one MD-UV380 vendor file listed in [codec-mduv380.md](codec-mduv380.md) as a reference finds them in V4.05 by content, then the code window by the table island's references; the emulator check of the MD-UV380 pages confirms the windows |
| R6 | Bootloader entry keys and DFU loader (expected the MD-UV380's: PTT and the top side key at power on) | on the radio |

## 5. Sources

The vendor image above, read with `tools/rfoundry` (container, key) and `arm-none-eabi-objdump`. Nothing from other
open firmware for this radio is used (docs/CLEANROOM.md).
