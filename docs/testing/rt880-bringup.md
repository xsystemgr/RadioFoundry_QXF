<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# RT-880 bring-up plan

Status: private FM receive target builds and passes model tests. No hardware test has been run.
The input facts and unknowns are in [Hardware investigation](../spec/hardware-rt880.md).
Follow [Adding a radio](../dev/adding-a-radio.md). The owner explicitly waived the
[clean-room role separation](../CLEANROOM.md) for the closed experiment on November 1, 2026.

## First milestone

Boot independently written code, establish a diagnostic link, show a RadioFoundry screen,
read the controls and receive analog FM with transmission inhibited. Use the existing generic
core and HAL. Pin assignments, clock settings, RF tables and display commands must have
documented sources before they become board code.

## Ordered work and acceptance

| Step | Work | Evidence required to proceed |
|---|---|---|
| 1. Identify the unit | Record model suffix, PCB revision, readable chip markings and oscillator markings | MCU, RF, display, storage and GPS variant recorded in the hardware specification |
| 2. Establish recovery | Determine loader protocol, image address, erase geometry and backup/restore procedure | Validated application/codeplug/calibration backups and a documented recovery route |
| 3. Establish the platform | Select or implement startup, clocks, time base, interrupt handling and RTOS integration | Datasheet-backed memory map; non-transmitting diagnostic boot on the actual unit |
| 4. Establish RF-off state | Map PA supply/keying, RF switch and power latch; implement `board_rf_safe()` | Verified transmitter inhibition during boot, faults and resets |
| 5. Board basics | Display, controls, power/battery and host logging | Known-good screen, key scan and stable diagnostic output |
| 6. Storage | Read factory calibration and define separate settings allocation | Read-only validation first; writes cannot overlap calibration or recovery data |
| 7. FM receive | RF bus, frequency programming, audio route, RSSI and squelch | Known test signal received at measured frequency and bandwidth |
| 8. FM transmit | PA control, power calibration, deviation and subtones | Explicitly authorized RF test into a dummy load; measured output, deviation and shutdown |
| 9. Optional functions | GPS, broadcast/shortwave tuner, APRS and host protocols | Separate feature-level host tests and opt-in hardware records |
| 10. Registration | Complete board registry, build matrix, flashing tools and documentation | Required checks pass and the installed hardware revision is accurately represented |

Steps are not completed by preparing a binary. Keep this page at research status until actual
results exist. Do not add a flashable release or claim that a different model's binary is compatible.

## Feature scope

| Feature | Initial scope |
|---|---|
| Analog FM | First radio milestone; new board glue required |
| UI, VFO, scanning and settings | Reuse generic code after display, controls and storage work |
| AM/SSB reception | Later investigation tied to the actual RF/tuner hardware |
| GPS and APRS | Later; depends on variant and audio access |
| DMR | No established modem/vocoder path; do not set `CAP_DMR` |
| M17 or other software digital voice | Separate feasibility work: sample path, modulation bandwidth, timing, CPU and RAM must be measured |

Adding a menu or codec alone does not establish digital RF capability. The first port can be
useful as an analog firmware even if no digital mode is feasible.

## Validation before code integration

- Host-test new drivers against simulated interfaces.
- Use `BOARD_NOT_WRITTEN` for confirmed hardware whose driver is missing; reserve
  `BOARD_UNSUPPORTED` for established hardware limitations.
- Run the registry, generic-document, link and license checks when adding board files.
- Run the required build and feature matrix after implementation exists.
- Record hardware results with the project's bring-up tools once a board target exists.

No flashing or radio transmission is authorized by this document. Hardware operations follow
the owner's specific test request and the repository's opt-in HIL procedure.


## Private UI build checkpoint (before receive integration)

An experimental standalone build now exists at
[boards/rt880](../../boards/rt880/README.md). It reuses the existing RadioFoundry
core/UI with a new AT32F423 platform and GPIO serial LCD backend. It is deliberately
not registered for releases or automatic flashing. The host test covers TX and
storage rejection, keypad scanning and the serial display contract. Link usage is
91,852 bytes flash and 44,456 bytes SRAM. The candidate device and memory map still
need independent physical confirmation. A CPU startup model is not completion of
steps 1-7 above; there has been no physical boot or receive test.


## Private receive checkpoint

The standalone target now implements FM RX with the existing BK4819 driver and
RadioFoundry core squelch. Host tests check both pin buses, ID refusal, VHF/UHF
routing, tuning/status/audio, TX/PA restrictions and bounded fault shutdown. A CPU
model runs 60 core radio-task iterations and opens/closes the speaker for modeled
carrier/loss. No real signal has been received on a physical unit during this work.

The owner confirms nicFW 5.14.x is running now. Before replacement, retain a full
RMS external-storage backup and the exact working nicFW application. Verify the
PTT-at-power-on loader entry without sending write commands and retain the same
model profile that previously installed working nicFW; do not select a profile
solely by interpreting the serial suffix. Storage backup restoration requires
reinstalling working nicFW first, because this lab target has no RMS link.

The first physical test must record boot/display, both RF IDs, default-channel
tuning, noise/RSSI and actual audio with a known signal, plus a monitor/noise test
and signal-loss squelch closure. Observe unexpected current or heating and power
off if seen. The image still has no transmit implementation; no RF transmit test
is part of this checkpoint. Do not change this record to hardware-tested until
those observations actually exist.
