<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# RT-880 private FM receive experiment

This target compiles the existing RadioFoundry core, UI, VFO, channel list, scan,
settings and radio state machine with an AT32F423 platform and board HAL. VHF/UHF
receive is implemented and model-tested. The owner confirms the first RX binary
boots and displays the UI. The owner confirms fix2 displays in the correct orientation but remains slow.
The indicator reacts to a transmitted signal, with no audible speaker output.
Monitor has not been tried. Fix4 with corrected front-key mapping awaits a physical test; demodulation and audio
remain unverified. This is not a supported release. The owner waived clean-room role separation for the
closed experiment on November 1, 2026. No reference firmware code or tables were
imported; the existing MIT BK4819 driver supplies the chip implementation.

There is deliberately no `board.mk` or flasher registry entry. Run from the root:

```sh
make -f boards/rt880/Makefile -j8 image test stack-check
```

Requirements: the pinned Arm GNU Toolchain and FreeRTOS submodule. The output is
`build/rt880-lab/rfoundry_rt880_rx_keys_fix4_UNTESTED.bin`. No command opens a serial port.
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

The existing ST7789 driver renders a portrait 120x160 indexed UI scaled 2x to
240x320, with 9,600 bytes of frame buffer. Fix2 uses MADCTL 0x00: bit 5 must
be clear for this 240-column, 320-row window. The previous 0x20 exchanged the
axes; the owner confirmed the rotated/noisy result. Fix2 needs owner confirmation. The backlight is on/off. Fix4 maps the inferred front legends as follows; owner confirmation is pending:

| Front key | Raw matrix bit | RadioFoundry action |
|---|---|---|
| 0 | 7 | Digit 0 |
| 1 | 0 | Digit 1 |
| 2 | 4 | Digit 2 |
| 3 | 8 | Digit 3 |
| 4 | 1 | Digit 4 |
| 5 | 5 | Digit 5 |
| 6 | 9 | Digit 6 |
| 7 | 2 | Digit 7 |
| 8 | 6 | Digit 8 |
| 9 | 10 | Digit 9 |
| Green Menu/OK | 12 | Select / open menu |
| Up arrow | 13 | Up; hold repeats |
| Down arrow | 14 | Down; hold repeats |
| Orange Exit | 15 | Back; hold returns Home |
| * / lock | 3 | Quick settings; hold locks/unlocks from Home |
| # V/M | 11 | Channel/VFO toggle |

The electrical scan is column-oriented relative to the front digits: its first
four positions are 1, 4, 7, *. Rows are PB10, PB2, PB1, PB0; columns PC10,
PC11, PC12, PD2. Fix4 replaces the old temporary positional actions. Monitor is
now selected in Quick settings; no digit key is used as a held Monitor key.
ADC side keys remain unimplemented.

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
RF performance, interrupt/scheduler stress, peak heap and physical stack usage
remain unmeasured.

MCU family evidence is stronger, but no software model proves the owner's exact
silicon/PCB, analog wiring, calibration accuracy or bootloader erase behavior.
Keep a full device-specific backup and the currently working nicFW binary.
The owner confirms the loader is accessible. See the
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


## Slow-display correction and preview

The old GPIO byte sender performed two DWT-based microsecond waits per bit.
At the fixed 8 MHz startup clock this made full-screen clearing/drawing slow.
Those waits are removed from the LCD sender; RF bus timing is retained. The
backlight/control pin is enabled before LCD reset/commands. No PLL change is made.
A compiled-function model sends the same 0xA5 byte with 135 instructions instead
of 559 under its one-cycle-per-instruction DWT model. This is not a measured
wall-clock speedup on the radio.

The taller frame adds 160 bytes of row metadata while retaining the same 9,600
pixel-buffer bytes. Host tests cover its 240-pixel physical row, byte order,
orientation command and 2x scale. Keypad FM tuning flows, few-key flows and screen
crawls pass at the custom 120x160p indexed geometry. The existing registered
classes still pass their full layout checks. The experimental narrow portrait
profile is not registered as supporting every digital/dual-watch screen.

The [FM](preview/fm.png), [VFO](preview/vfo.png), [menu](preview/menu.png) and
[settings](preview/settings.png) previews are rendered from the real UI host
simulator. They show sample content and its full-feature menu configuration,
not a capture from the radio or a promise that every pictured feature is enabled.

## Fix2: orientation and continued slow response

The owner photo is confirmed to show the first portrait binary. It is still
rotated with a noisy strip; refresh is only slightly faster. Using a reference
MADCTL value without matching our window dimensions was incorrect.

Fix2 clears MADCTL MV and tests CASET 0..239 and RASET 318..319 for the last
logical row. The LCD sender now combines clock-low and data into one atomic
GPIO set/clear write and unrolls eight bits. It touches only PB3/PB5, leaves
interrupts enabled and keeps the 8 MHz clock and all RF delays. The compiled
sender model verifies all 256 byte values and takes 52 instructions per byte
instead of the previous portrait build's 135. This is not a measured refresh
speedup; software pixel transfer still limits full-screen updates.

## Fix3: owner confirms orientation, reports no audio

The owner confirms fix2 orientation appears correct. It remains slow, and the
signal indication reacts while another radio transmits, but the speaker is
silent. This is not yet proof of intelligible reception. Monitor was not tried.

Fix3 uses the internal HICK at 48 MHz without a PLL. The startup sequence sets
flash performance wait states, divides HICK before changing its selection,
switches away from and disables the inherited PLL, then selects HICK48 and
updates SystemCoreClock. DWT microseconds/milliseconds and RTOS ticks use the
same frequency. The reference manual's register tables put HICK_TO_SCLK in
MISC1 bit 14, despite a conflicting MISC2 name in section 4.1.1's prose.
See [AT32F423 RM](https://www.arterychip.com/download/RM/RM_AT32F423_EN_V2.03.pdf),
sections 4.1.1, 4.3.23, 4.3.24 and 5.8.1. Only flash read-performance bits are
changed; no programming, erase or calibration writes occur.

RF data direction is configured once after read turnaround rather than once
per outgoing bit. Serial edge delays are retained. The LCD orientation stays
at fix2's 0x00; the panel frame-rate register is unchanged. The expected speed
benefit comes from CPU throughput, not an assumed panel refresh setting.

A software volume hook now drives the existing BK4819 AF DAC gain helper;
50 percent is the initial level (0x48 = 0xB3B8 rather than 0xB3B3). Zero closes
AF and the speaker gate; restoring volume preserves the core's audio request.
This is the core's Volume control; physical knob wiring is unverified.
It is a gain/control improvement, not a confirmed explanation for the silence.

Diagnostics now show CPU MHz, last AF/AF-gain register readbacks, Audio OPEN or
MUTE with volume percent, and the last RSSI/noise sample. OPEN describes the
software's gate command, not measured amplifier output. Try Squelch 0 in Quick
settings, with nonzero Volume. If silent, record AF, gain, Audio and RSSI/noise
while a known FM signal is present. AF 6140 and a nonzero gain are expected
when the core opens audio; those values alone cannot prove the external path.

Native tests cover gain clamp, mute/unmute, retained audio requests, readbacks,
and bus direction changes, while retaining the PA/TX guards. The compiled
startup/RF model passes from reset and a modeled inherited PLL configuration.
Physical speed, RF demodulation, speaker routing and volume need owner checks.

## Fix4: front-key mapping

The owner reports menu buttons do not match the legends. The previous key map
assigned navigation/digits to arbitrary electrical positions. The reference
key decoder translates the first scan column to 1, 4, 7, *, the next to 2, 5,
8, 0, the next to 3, 6, 9, #, and the last to Menu, Up, Down, Exit. Decimal entry
and navigation/exit consumers support those meanings; the front legend labels
remain inferred until the owner presses them with this image.

Fix4 changes only the board key map: it retains the RadioFoundry menu/Quick/
key-lock behavior, fix3 clock/audio diagnostics and all RX-only restrictions.
Arrow long actions now match their short actions so held navigation repeats.
The native board test drives every pin position through the actual input
engine and checks all 16 short actions, arrow repeats, long * and long Back.
No ADC side-key or PTT binding is added. Access Monitor from Quick settings.

## Owner confirms keys; audio remains silent at squelch zero

The owner confirms the fix4 physical keys work. Speaker silence persists with
squelch disabled. An isolated run of the reference receive path confirms AF
0x6140, receiver enables 0xBFF1 and the previously observed PF8-high/PC4-low
audio-open command. The port already requests those values.

The reference DAC Gain setting controls both Gain2 (0x48 bits 9:4) and DAC gain
(bits 3:0); observed settings 0/7/15 produce B010/B1E7/B3FF. The port's software
volume currently changes only the DAC gain over B3B0. Thus software Volume is
not the same control curve as the reference DAC Gain setting. This difference
alone does not establish the cause of complete silence.

Only direct calls to the identified ADC reader for side keys (channel 12) and
battery scaling (channel 7) were found. The earlier suggestion of an ADC-backed
volume knob was not established. Analog potentiometer wiring is plausible,
but cannot be proved from the image; a schematic or measurement is needed.

Other observed differences include receiver power 0x37 = 1F0F in the reference
versus 1D0F in the port (RF LDO voltage bit), and default AF filters 0x2B = 0000
versus C000. The shared chip driver's profile originated on another board.
These are investigation leads, not permission to copy its initialization table
or assert that either difference explains silence. No new binary is issued for
this documentation checkpoint.

Next hardware evidence: with squelch zero, software Volume 100 and the physical
knob at a moderate position, record both RF IDs, AF/gain readbacks and commanded
Audio OPEN/MUTE. At software Volume 100 the current expected gain is B3BF;
AF is 6140 when open. If these do not match, investigate bus/state handling.
If they match and there is no noise, investigate the analog AF/speaker path and
remaining device-specific receiver setup. The radio is not physically attached
to the development environment.

Register meanings: [Beken register list](https://alfaexploit.com/files/BK4819V3Registers_List_20201218.pdf),
registers 0x30, 0x37, 0x47 and 0x48.
