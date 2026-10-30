<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# MD-2017 bring-up checklist

The MD-2017 port (`make BOARD=md2017`) is the UV380 board family's code with the MD-2017's pins (`UVF_MD2017` in
`boards/uv380_family/pins.h`) and its data in `boards/md2017/board.c`. Everything below comes from reading TYT's
V4.05 image ([hardware-md2017.md](../spec/hardware-md2017.md)). Nothing has run on a radio yet.

What this first image does: receiver A (the AT1846S at 0x2E, the one that also transmits) on its own bus, receiver B's
supplies and audio switch off, the MD-UV380's keypad matrix, calibration reading and power tables, and **no
transmit** (`tx_block`: "MD-2017 bring-up: RX only"; `MD2017_DEFS="-DMD2017_TX=1"` lifts it once stage 5 passes).

## 0. Before anything

1. Back up the radio with the firmware it runs now: the codeplug, and the calibration with that firmware's CPS (an
   OpenMD2017 radio: the OpenGD77 CPS calibration backup). RadioFoundry keeps security registers 1-3 untouched
   (board.toml `[[flash.protect]]`), but the backup is the only way back if something else is lost.
2. Build: `make BOARD=md2017`. With `TYT2017-UVCSV-V4.05.bin` in `codecs/`, the build also writes `_codec.bin` with
   the codec merged in (stockfw.py profile, codec map `md2017/md2017-v405`).
3. Flash through the TYT bootloader (PTT and the side key above it held at power on), as the MD-UV380.

## 1. It boots

| Check | Pass | If not |
|---|---|---|
| The backlight comes on (PE4) | the screen is lit | PE4 is the TYT image's backlight (on / off); a dark but drawn screen points at the pin |
| The screen draws | the home screen | the panel type (security register 3) or the FSMC timing: the MD-UV380's are used |
| USB enumerates as "MD-2017" | the debug link answers `VERSION` with board=MD-2017 | the clock (`hse` on the diagnostics page) |

## 2. Keys and knobs

The keypad matrix is the MD-UV380's and may be wrong. Press each key with the diagnostics page open; note what the UI
does. The diagnostics page's last two lines show the inputs the TYT image reads whose use is unknown
(`A13:1 A15:1 ...`, three lines with E11 (PTT) and E12, 1 = high; PE12, the MD-UV380's external PTT, is not taken as PTT here): press each side key and turn each knob, and note
which digit moves.

## 3. Receiver A, FM

| Check | Pass | If not |
|---|---|---|
| The diagnostics page shows the HR_C6000's read-back (`c6000`) | 0x80 in FM | the control bus: CS PE2, CLK PC13, MOSI PE5, MISO PE3 (hardware-md2017.md 3.1) |
| RSSI moves with a signal on a VHF and a UHF channel | a few dB per 10 dB step | the AT1846S bus (I2C1 on PB6 / PB7, 0x2E) or the RX supplies (PC9 UHF, PA5 VHF) |
| Audio on a strong FM signal | heard | PD9 (receiver A's audio switch) and the amp (PB8, PB9, as the MD-UV380) |

## 4. DMR receive

With the codec merged: a DMR voice call is heard. If FM works and DMR doesn't, the codec descriptor (diagnostics:
codec present) or the HR_C6000's voice port (SPI2, as the MD-UV380).

## 5. Transmit (a later image)

Not in this image. Before it: the calibration layout of this radio (two receivers: the MD-UV380's layout may not
hold), the PA band select (PC6) and the power tables, each read from the TYT image first.

## 6. Receiver B

Later: chip 0x71, its supplies PA8 / PA2 and audio switch PD8 (hardware-md2017.md 2), for dual receive.
