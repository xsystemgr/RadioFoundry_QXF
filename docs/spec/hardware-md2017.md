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
windows are in 1.1.

### 1.1 The codec in V4.05

Found by carrying the MD-UV380 codec maps (`codec_maps/mduv380/`, addresses and offsets only) over to this image: at
every CODEC_SIG word, a map fits when its self refs point back into the candidate by the map's offsets and its table
refs into flash. One candidate fits, with the P18.11 CSV-GPS map (code length 0xF6E2, the 18.xx shape). From the code's
own refs each table object and island gets its new address; then every island ref and every BL from an island into the
code was checked in the image: all of them hold (measured, 0 mismatches).

| window | flash | length | sha256 |
|---|---|---|---|
| code | 0x08075808..0x08084EEA | 0xF6E2 | `dda72c91f42456deac38f596fdfdf5f04c01cac071e177b750f4d7006cbeec49` |
| tables (the codec's data, first object to the end of the last) | 0x080D2AF4..0x080E4A4A | 0x11F56 | `87d676a7e1b36774a18511dcb10c46c633c88baa206531c2560f3c4f2e013ca4` |

* Not byte-identical to P18.11 with the addresses restored (its code SHA-256 doesn't come out), so it needs a map and a
  profile of its own.
* The table objects don't move as one block against P18.11 (seven different shifts, -0x4EB8 to -0x4B80), so the span
  is 0x338 bytes shorter than P18.11's cut. 46 of the 52 objects are byte-identical to the same objects in the MD-UV390
  S119.38 file below; the other six differ in a few bytes (object bounds "to next object", ?).
* **Emulator (measured).** Only these two windows mapped (the rest of each page zero), the hosted init sequence
  ([codec-mduv380.md](codec-mduv380.md) "Entry points"), then 50 frames of a 1 kHz tone at 8000 peak through encode and
  decode: no fault, 2000 zero crossings per second (1 kHz), peak 8611. The 49-bit frames and the PCM are bit-identical
  to the S119.38 codec's run the same way.

Cross-check build: `MD-UV390-AESGPS-REC-S119.38.bin`, 1365504 bytes, SHA-256
`c99a13b4e0ababe534eafa23fafca636fe1d87187ec3ef49360ddcf7f2f5f657`, application 0xF0000, not yet in
codec-mduv380.md's list. Its codec fits the 10 W S219.74 GPS-REC map: code 0x0808A49C + 0xF38E (`6f96ad22...fbda31`),
tables 0x080E7C1C..0x080FA396 (0x1277A, `cba4a2ea...3559f7`), every island ref and branch checked.

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
is on PE4 (3.1). The same routine writes PC4, PC5 and PC6 low (PC6 is the PA band select on the MD-UV380,
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
| PC13 | out | — | **HR_C6000 control SPI clock** (3.1) |
| PD0, PD1, PD4, PD5, PD12, PD14, PD15, PE7-PE10 | AF12 FSMC | LCD bus | same |
| PD6, PD13 | out | LCD CS, LCD reset | same |
| PD8 / PD9 | out, opposite levels | backlight / RX audio mux | **receiver switch B / A** (2.1); the backlight is PE4 |
| PD10, PD11 | in, read | — | new inputs (a second knob? ?) |
| PE0, PE1 | out | LEDs | same |
| PE2 | out | C6000 CS | same |
| PE3 / PE5 | in / out | C6000 CLK / MISO | **C6000 MISO / MOSI** (3.1) |
| PE4 | out | C6000 MOSI | **LCD backlight** (3.1) |
| PE6 | out | C6000 PWD | same (?) |
| PE11, PE12 | in pull-up | PTT, external PTT | same |
| PE13, PE15 | in pull-up | — | new inputs (keys? ?) |

### 3.1 Backlight, HR_C6000 control bus, inputs

| Item | What the image does |
|---|---|
| LCD backlight: **PE4** | set high from many places (key and screen activity, 31 call sites); a tick routine counts a RAM counter down while a settings byte enables the time-out, and drives PE4 low when it reaches 0. Plain on / off: no TIM or DMA writes a GPIO register in the image, so no PWM dimming was found (image) |
| HR_C6000 control SPI, bit-banged | **CS PE2** (low around each transfer, inside a mutex), **CLK PC13**, **MOSI PE5**, **MISO PE3** (input); 8 bits MSB first, data out set before the clock edge, data in read after it. The register write (0x0804BC80: 0x04, register, value) and the other C6000 accesses go through it (image). On the MD-UV380 this bus is CLK PE3, MOSI PE4, MISO PE5, so the MD-2017 board needs its own pins here |
| PE6 | written (the MD-UV380's C6000 power-down pin; same role assumed, ?) |
| PE15, PB10 | read in the same routine as the channel encoder (PE14, PB11), each into its own RAM byte: a second two-pin input next to the encoder (?) |
| PD10, PD11 | read as a pair and compared as a 2-bit state (`& 3`): quadrature-like (?) |
| PA13, PE13 | each read three times in a row and compared: debounced keys (?) |
| PB0, PB1 | inputs read in one menu path (?) |
| PB2 | output, written in the RX / TX switching (?) |

## 4. Open questions (next steps)

| # | Question | How |
|---|---|---|
| R1 | ~~The receivers' audio~~: PD9 A, PD8 B, both on in FM (2.3); confirm on the radio | |
| R2 | ~~DMR input~~: the selected receiver alone, either one (2.3); confirm on the radio | |
| R3 | ~~Each receiver's band supplies~~: done (2.2) | |
| R4 | ~~Backlight and the C6000 bus~~: PE4, and CS PE2 / CLK PC13 / MOSI PE5 / MISO PE3 (3.1). Still (?): PA13, PA15, PB0-PB2, PB10, PD10, PD11, PE13, PE15 (keys and a second knob likely) | on the radio, with a pin-state readout |
| R5 | ~~The codec windows of V4.05~~: found and run in the emulator (1.1). Still to do: its codec map (`codec_maps/md2017/`) and the profile in `stockfw.PROFILES`, whose placement waits for the board's memory layout | with boards/md2017 |
| R6 | Bootloader entry keys and DFU loader (expected the MD-UV380's: PTT and the top side key at power on) | on the radio |

## 5. Sources

The vendor image above, read with `tools/rfoundry` (container, key) and `arm-none-eabi-objdump`. Nothing from other
open firmware for this radio is used (docs/CLEANROOM.md).
