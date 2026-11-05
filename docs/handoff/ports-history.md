<!-- SPDX-License-Identifier: MIT -->
# RadioFoundry_QXF — ports history & handoff

Private fork of **RadioFoundry** (open-source MIT DMR radio firmware), forked to
**`xsystemgr/RadioFoundry_QXF`** to carry three separate radio ports. This doc is the single place a new
session (Claude Code or chat) should read first to pick up where things stand.

Goal, in the owner's words: bring the fork to several radios — **AM receive on the DM-32UV**, the **TYT MD-2017**,
and the analog-only **RT-880 (iRadio)**, with **HF reception on the RT-880** via its Si4732 (as a second VFO, later
SSB). The radio is a licensed-ham hobby project.

> Status legend: ✅ built & believed working · ⚠️ built, **UNTESTED on hardware** · ⛔ not started

---

## Branch map

| Branch | Radio | What it holds |
|---|---|---|
| `main` | — | Upstream base (tracks `origin/main` = RadioFoundry/RadioFoundry). |
| `am-flag-uhf-airband` | **DM-32UV** | AM receive beyond the VHF air band + 225–400 MHz military UHF air band; per-channel AM flag; FD6818 diag. |
| `md2017-port` | **TYT MD-2017** | UV380-family board for the MD-2017; dual watch with a second FM receiver; keypad/codec for V4.05. |
| `research/rt880-port` | **RT-880 (iRadio)** | Analog-only port: FM RX, audio fix, **Si4732 HF/SSB second VFO**, HF usability. ← most recent work. |

Two remotes: `origin` = upstream (RadioFoundry/RadioFoundry, **no push access**), `fork` =
`xsystemgr/RadioFoundry_QXF` (**push here**). All port branches live on `fork`.

---

## Port 1 — DM-32UV (branch `am-flag-uhf-airband`)

**What was done**
- AM receive added beyond the stock VHF air band, including the **225–400 MHz military UHF air band**.
- **Per-channel AM flag** so AM is selectable in the channel config.
- AM squelch intent: level 1 at **−90 dBm** (VHF air band) and **−100 dBm** (milair).
- DM-32 diagnostics read back FD6818 registers while AM plays; codec map for the NRF 049 build.

**Status:** core AM-RX + airband work committed. Bench-verification of the exact squelch dBm targets is the open item.

---

## Port 2 — TYT MD-2017 (branch `md2017-port`)

Board is modeled as part of the **UV380 family** with MD-2017 pins, for TYT image **V4.05**.

**What was done**
- Board, codec map and profile for V4.05; codec windows found and checked in an emulator.
- Hardware notes from the vendor image: HR_C6000 control SPI on **PE2/PC13/PE5/PE3**, backlight **PE4**; per-receiver
  band supplies; receiver audio switch **PD9 (A) / PD8 (B)**.
- **Dual watch with a second physical receiver** (two FM channels at once): receiver B inited on first dual-watch
  tune (not at power-up), polled every 100 ms (200 ms while A plays); the receiver not being heard is muted via
  **AT1846S reg 0x30 bit 7**.
- Keypad: live (row, column) readout on the diagnostics page. **PE12 is NOT PTT** (the TYT image treats it as a key);
  PTT pin candidates are shown on the diag page — **PTT pin still to be confirmed**.

**Status:** dual-watch second-receiver path and bring-up committed. Open: confirm the PTT pin; on-air checks.

---

## Port 3 — RT-880 / iRadio (branch `research/rt880-port`) — most active

Analog-only radio (no DMR). **AT32F423** MCU (48 KB RAM, 256 KB flash, app base `0x08002800`, SP `0x2000BFF8`),
**two BK4819** RF chips, a **Si4732** HF/broadcast tuner, ST7789 240×320 display driven at 120×160 logical scale-2.

### History, in order
1. **UI bring-up** on the RadioFoundry core with RF and storage denied; portrait LCD fixed; keypad legends mapped to
   the real electrical scan order.
2. **FM receive** implemented (TX denied at first), with RF-shutdown and a PC13 reset workaround.
3. **Silent/faint audio fixed** — the root cause: the firmware received on the wrong BK4819. There are **two** BK4819
   chips; the audio amplifier is fed by **chip B** (bus_b: PF6 clk / PA8 cs / PC9 data). Chip A (bus_a: PC8/PC7/PC6)
   is the idle "control". Fix: `receiver = bus_b`, `control = bus_a`. **Both chips are present** — `init()` only
   starts if *both* return `BK4819_ID`; neither is disconnected. (This answered the owner's early "is the 2nd BK
   disconnected?" — no.)
4. **AM airband RX** (108–137 MHz) via BK4819 (reg 0x47 = 0x6740, AFC off).
5. **TX unlocked** like the factory sequence (STATE_TX_MAIN, PA bit7, PF0 band-path relay); **airband kept RX-only**
   (aviation safety — non-negotiable). VHF/UHF TX allowed.
6. **Dual watch + DM-32-style fonts** as a "second VFO" (an earlier default build).
7. **Si4732 HF receiver as a second VFO** (the big recent effort) — see below.

### Si4732 HF (the current feature set on this branch)
- **Driver** `drivers/bcast/si4732/` — `si4732.c/.h` (AN332 command set) over the bit-banged **i2c_gpio** bus, and
  `si4732_hal.c/.h` as the board's `hal_bcast_t`. Wired on **PC0 SCL / PC1 SDA**, power **PC5**, I2C addr **0x11**.
- Added as a **band** `{0.15–30 MHz, RX only}` so the VFO can tune HF; the core routes a VFO inside the bcast range to
  the Si4732 automatically (it appears as the second VFO).
- **Spectrum split:** Si4732 keeps **0.15–24 MHz** (AM/SW, and SSB on ham segments); **24–30 MHz** (CB 27, 10 m FM)
  is routed to the **BK4819 for narrowband FM** instead — the Si4732 cannot demodulate NBFM on HF, and the BK4819's
  VHF front end is deaf that low so only strong/near signals come through (hardware limit, not firmware).
- **Audio-path fix:** both receivers share one speaker gate (**PF8** gate / **PC4** mute). The core drives the two
  paths in turn, so the gate is now owned jointly in `radio.c` (open while *either* wants audio; each source muted
  separately). This is what made HF silent before.
- **AM front end opened up** for weak HF: soft-mute off, AVC max, 4 kHz filter, full RX volume. **Live RSSI** read via
  RSQ_STATUS (previously latched and stuck at −107 dBm — fixed).
- **SSB (LSB/USB)** via the Si4732 patch (`CONFIG_SI4732_SSB`): auto sideband by ham convention (LSB <10 MHz, USB
  above), with a **manual AM/LSB/USB override** and an **SSB fine-tune (BFO, ±900 Hz / 20 Hz steps)**.
  - The **SSB patch** (`drivers/bcast/si4732/si4732_ssb_patch.h`, ~15.8 KB) is **third-party** (Silicon Labs /
    V.Afonkin via the PU2CLR SI4735 library), **personal / non-commercial** use only. It is committed in this private
    fork; the driver pulls it in with `__has_include` so the build still compiles (AM-only, with a `#warning`) if the
    file is ever removed. See `drivers/bcast/si4732/README.md`. Credited in `platform/at32f423/THIRD_PARTY`.
- **HF usability:** 1 kHz VFO step on the fine HF grid; the real demod (**AM/LSB/USB**) shown on screen; **Quick-menu
  tiles** "Mode" (cycle Auto/AM/LSB/USB) and "Fine" (BFO, edit with up/down); **A/B VFO** switching on **hold-#**
  (turn on Dual watch from the menu first); smooth (anti-aliased) fonts.
- **Diagnostics** (Diagnostics screen): line 4 = Si4732 GET_REV probe ("Si4732 fwX.Y" if the chip answers, else "no
  I2C PC0/1/5?"); line 5 = live "HF <kHz> R:<rssi> AM/LSB/USB".

### RT-880 status
- ✅ FM VHF/UHF RX, audio, TX (airband RX-only).
- ✅ Audio-path fix; AM/SW on the Si4732; 1 kHz step; modulation display; Quick-menu Mode/Fine; A/B VFO.
- ⚠️ **SSB UNTESTED on hardware.** AM/SW audio and the audio-path fix are the confirmed-working parts.
- ⚠️ CB 27 FM via the BK4819: works as NBFM but the front end is deaf at 27 MHz (near-field only).
- ⛔ **PTT pin not mapped** (TX works but the PTT key pin is unconfirmed — use the on-screen pin-finder: hold PTT and
  watch the diag port lines for a bit that drops 1→0).

---

## Building & flashing (RT-880)

From the repo root (needs the ARM toolchain `arm-none-eabi-gcc` on PATH or under `~/opt/arm-gnu-toolchain-*`):

```
make -f boards/rt880/Makefile HF=1 image     # HF build (Si4732 + dual-watch; drops DM-32 fonts to fit)
make -f boards/rt880/Makefile                 # default build (dual-watch + DM-32 fonts, no Si4732)
make -f boards/rt880/Makefile test            # host unit tests (board, radio)
make -f boards/rt880/Makefile test-hf         # host unit test for the Si4732 driver (mock chip)
make -f boards/rt880/Makefile HF=1 stack-check
```
Flash the `.bin` at **`0x08002800`** (SP `0x2000BFF8`). RAM on the HF build sits ~98.9 %; `stack-check` passes with
~60 bytes free on the idle task — keep an eye on it when adding features.

The RT-880 Makefile is intentionally **not** in the release/flasher registry (it is a lab target). The `HF=1` and
default builds use **separate build dirs** (`build/rt880-lab-hf` vs `build/rt880-lab`) so their objects, compiled with
different `CONFIG_BCAST_FM`, never clobber each other.

---

## Suggested next steps
- **RT-880:** bench-test SSB (tune a ham SSB frequency, use Quick-menu Mode + Fine); confirm the PTT pin; then a PR
  from `research/rt880-port`.
- **MD-2017:** confirm the PTT pin; on-air dual-watch checks.
- **DM-32UV:** verify the AM squelch dBm targets on the bench.

## Key reference docs in the repo
- `docs/handoff/rt880-audio-experiment-log.md` — the full RT-880 audio-fix diagnostic trail.
- `drivers/bcast/si4732/README.md` — the Si4732 driver and the SSB-patch licensing/how-to.
- `boards/rt880/README.md` — RT-880 board notes (RF B is the receiver, etc.).
