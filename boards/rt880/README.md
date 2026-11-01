<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# RT-880 private FM receive experiment

This target compiles the existing RadioFoundry core, UI, VFO, channel list, scan,
settings and radio state machine with an AT32F423 platform and board HAL. VHF/UHF
receive is implemented and model-tested. No physical radio test has run. This is
not a supported release. The owner waived clean-room role separation for the
closed experiment on November 1, 2026. No reference firmware code or tables were
imported; the existing MIT BK4819 driver supplies the chip implementation.

There is deliberately no `board.mk` or flasher registry entry. Run from the root:

```sh
make -f boards/rt880/Makefile -j8 image test stack-check
```

Requirements: the pinned Arm GNU Toolchain and FreeRTOS submodule. The output is
`build/rt880-lab/rfoundry_rt880_rx_UNTESTED.bin`. No command opens a serial port.
The candidate image address is `0x08002800`; loader erase boundaries are unverified.

## Receive implementation

- RF A: PC8 clock, PC7 select, PC6 bidirectional data. RF B: PF6 clock, PA8 select,
  PC9 bidirectional data. Data input is floating during read turnaround. Each
  frame preserves interrupt masking and leaves both selects inactive.
- Both chips must return BK4819 ID `0x4819` before initialization. A mismatch
  refuses receive and leaves audio muted. Diagnostics show both raw IDs.
- Before peripheral startup, the ES0010 PC13 workaround requests one software
  reset only when POR is set and the software-reset flag is clear. The flags
  prevent a reset loop; no calibration or backup-domain erase is performed.
- The existing BK4819 driver has a new receiver-only initialization entry point.
  It never writes nonzero PA level or GPIO outputs and never enables TX DSP.
  The ordinary main/LO initialization paths retain their previous behavior.
- Only RF A receives. RF B stays in state zero with PA level zero; its GPIO word
  is zero during route changes, then `0x10` for VHF RX or `0x20` for UHF RX.
  It is never used as a synthesizer/LO transmitter. This board differs from the
  existing superheterodyne board supported by the same generic driver.
- Initial bands: 136-174 and 400-480 MHz FM, 12.5/25 kHz bandwidth. No HF, AM,
  broadcast, CTCSS/DCS, GPS or APRS in this profile. Out-of-band, invalid bandwidth,
  non-FM and sub-audio requests fail. These limits describe this experiment,
  not the full hardware capability.
- RSSI and noise feed the existing RadioFoundry squelch. AF is muted before tuning.
  The observed speaker gate is PF8 high and audio mute is PC4 high. Opening audio
  enables FM AF, raises PF8, then lowers PC4; closing asserts mute first.
- TX always returns false, every band entry prohibits TX, and PTT is not bound.
  No TX/LO enable state, nonzero PA level or TX filter selection is issued.
- Fault shutdown uses bounded raw zero-value frames, without DWT delays or RTOS
  calls, to clear PA level, enables and GPIO outputs on both chips. PC4 is muted,
  PF8 is low, PC15 and PF0 are low. Electrical PA isolation is still unmeasured.

## Display and controls

The existing ST7789 driver renders a landscape 160x120 indexed UI scaled 2x to
320x240, with 9,600 bytes of frame buffer. Controller/orientation need hardware
confirmation. The backlight is on/off. Physical legends are not mapped yet:

| Raw matrix position | Temporary RadioFoundry action |
|---|---|
| 0 | Up / channel +1 |
| 1 | Down / channel -1 |
| 2 | Select; hold for the existing Quick menu |
| 3 | Back |
| 4, held | Monitor / open squelch |
| 5 | VFO/channel toggle |
| 6-15 | Digits 0-9 in order |

Positions are row-major. Rows are PB10, PB2, PB1, PB0; columns PC10, PC11, PC12,
PD2. These actions are positional diagnostics, not claims about printed legends.
The built-in fallback channels include 145.500, 146.520, 146.550 and 433.500 MHz.
In VFO mode, enter `145500` and Select for 145.500 MHz (the existing core entry
format). Up/down then step the VFO frequency.
No external storage operation is permitted, so settings remain volatile. Battery
measurement, ADC side keys, LEDs, power latch and programming link are absent.
A displayed zero battery value is unknown, not a calibrated measurement.

## Validation and remaining limits

The host board test covers input and serial display contracts and storage/TX
rejection. The radio test models both independent pin buses, read direction,
IDs, all receive band endpoints, tuning, status and audio, rejects unsupported
requests, and audits every frame against TX/PA/filter restrictions. Fault shutdown
is tested with delays forbidden. The generic BK4819 test also covers receiver-only
initialization while retaining all existing main, LO, tone and digital tests.

The build has zero compiler/linker warnings and passes all six static task-stack
checks. It uses about 94 KB flash and 44.5 KB SRAM, including a 12 KiB RTOS heap
and 4 KiB reserved for C heap/main/interrupt stack. A CPU model ran startup and
60 radio-task iterations: the RadioFoundry core tuned 145.500 MHz, opened the
speaker on modeled carrier and closed it on signal loss. The second chip stayed
idle and no PA level or TX state was written. RTOS heap free after task creation
was 1,608 bytes. Pixel bursts, delays and queue waits were modeled/bypassed; actual
RF performance, interrupt delivery, context switching, peak heap and physical
stack usage remain untested.

MCU family evidence is stronger, but no software model proves the owner's exact
silicon/PCB, analog wiring, calibration accuracy or bootloader erase behavior.
Keep a full device-specific backup and the currently working nicFW binary.
Verify the documented loader entry before the first physical experiment. See the
[hardware investigation](../../docs/spec/hardware-rt880.md) and
[bring-up record](../../docs/testing/rt880-bringup.md).

## Primary hardware references

- [nicFW author's MCU comparison](https://www.patreon.com/nicsure/posts/rt-950-pro-vs-rt-153711437): AT32F423 and BK4819 in the RT-880.
- [Author's schematic-based RF-path explanation](https://www.patreon.com/nicsure/posts/rt-880-is-very-133417003): second chip lacks a receive path.
- [Radtel's firmware/serial families](https://www.radtels.com/pages/software-download).
- [RMS backup and recovery instructions](https://github.com/nicsure/nicFW880_950_Docs/blob/main/RMS880.wiki/Firmware-Tab.md).
- [Official PC13 errata/workaround](https://arterytek.com/file/download/2228).
- [Official AT32F423 register definitions](https://github.com/ArteryTek/AT32F423_Firmware_Library/blob/master/libraries/drivers/inc/at32f423_crm.h).

Retain the source package's RadioFoundry, FreeRTOS and toolchain runtime notices
with any binary copies. Nothing from this experiment was published to GitHub.
