<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# MD-2017, DM-32 and fleet admin — session handoff

**Checkpoint: 2 November 2026.** Branch `md2017-port` on the fork
(`github.com/xsystemgr/RadioFoundry_QXF`), pushed. This note hands off the work done in this session so another
developer or AI can continue. Hardware-confirmed items are marked; everything else is built and host-tested only.

## 1. What was added this session

### Fleet administration over DMR (OTAP / OTAR) — new
Full feature, `docs/spec/otap.md`. One owner's radios share a 32-byte secret (set over USB, never on air); a command is
obeyed only with a valid HMAC-SHA256 signature and a counter above the last taken (replay protection).

- `core/crypto/sha256.c` — SHA-256 + HMAC (also host-tested; Python mirror is byte-identical).
- `core/dmr/otap.c` / `otap.h` — message format, tag, key encryption (pure, `tests/host/otapTest.c`).
- `core/dmr/otapSvc.c` / `otap_api.h` — the service: receive/verify, run, answer, PC console relay
  (`tests/host/otapSvcTest.c`). Sends are **retransmitted** `OTAP_SEND_TRIES` times (unconfirmed) so a target that
  missed the first burst still catches it; duplicates are dropped by the replay counter.
- Commands: STATUS, TALK_TG, RX_TG, ZONE_POS, PRIVACY on/off, SET_KEY (OTAR, key encrypted, band-gated).
- **Group** commands (SEND_GROUP): to a talkgroup, every fleet radio at once, unconfirmed, **no reply** (confirm with
  the counters).
- Per-radio policy (`SETTINGS_OTAP_SETTINGS` / `_KEYS`) gates settings and key changes, so OTAR is switchable off; up
  to two admin IDs restrict who may command.
- Debug link `DL_CMD_OTAP` (`debuglink.md` 3.23): provision (SECRET/FLAGS/ADMIN), console (SEND/SEND_GROUP/RECV),
  STATS (14 counters, for diagnosis), LOCAL_KEY (set+save this radio's privacy key over USB).
- `tools/rfoundry_debug.py` — `otap` command and the `otap_*` DebugLink methods; `rfoundry_bench_gui.py` — the
  "Στόλος (OTAP/OTAR)" tab (provision, console, "new privacy key" rekey, group checkbox, counters).
- `CONFIG_OTAP` = `CONFIG_DMR_PRIVACY && CONFIG_DMR_DATA_CORE`, overridable per board.

### Privacy transmit bands — new
`settings_t.privacy_band[8][2]` (kHz ranges): a loaded key encrypts our calls only inside them, elsewhere calls go out
in the clear (shown CLEAR/PrivRX). Default VHF/UHF PMR ranges, amateur bands left out. `CONFIG_PRIVACY_BANDS`
(= `CONFIG_DMR_PRIVACY`, per-board). Band-gate also guards OTAR.

### Panic wipe — new
`*` + `#` held 10 s → `ACT_ZEROIZE` → factory reset (settings incl. keys and the fleet secret, and the codeplug) then
reboot (`core/input/input_engine.c`, `core/ui/ui_app.c`, `tests/host/inputTest.c`).

### Settings store
`settings_t` grew to a 3rd page (520 B) for the fleet-admin block; both new groups are behind config so a RAM-tight
board pays nothing. `settings-store.md` and `settingsTest.c` updated for 3 pages (sizes 400 / 464 / 520 by config).

## 2. DM-32 (Baofeng DM-32UV)

- **AM receive**: the factory-derived airband profile is already the default (`RADIO_AM_MODE_STOCK`,
  `boards/dm32/radio_dm32.c`): FD6818 `0x43=0x0048, 0x47=0x6740, 0x13=0x03FF, 0x4A=0x5450`, reapplied each retune,
  restored on AM exit. Matches the factory sequence in the handoff (`DM32_AM_RadioFoundry_handoff.md`) register for
  register. **Not hardware-confirmed to produce audio** — needs an on-radio signal test.
- **SFR dropped, OTAP on**: the DM-32 keeps only ~0.5 KB RAM free. Turned `CONFIG_SFR` off (saves ~1.3 KB) and let
  `CONFIG_OTAP` + `CONFIG_PRIVACY_BANDS` default on (`boards/dm32/config.mk`, `board.mk`): the DM-32 is a full fleet
  member. Builds at 99.7 % RAM. A radio wanted as a single-frequency repeater keeps the default SFR build instead.
- **Codec**: the build has no DM-32 codec in `codecs/`, so `rfoundry_dm32.bin` has FM/AM but silent DMR voice. The
  stock file `DM32_049_20250905.bin` was provided this session — drop it in `codecs/` and rebuild for a
  `_codec.bin` with DMR voice. (Flasher can also merge at flash time: `--codec merge --stock DM32_049...bin`.)

## 3. MD-2017 (TYT) — keypad: OPEN

The port runs (FM RX, DMR RX via repeater, TX, dual watch — hardware-confirmed earlier). The **keypad matrix is the
MD-UV380's, unconfirmed**, and the owner reports **Menu/Back swapped with the P1/P2 side keys**.

- Added a `kbd R0:xx R1:xx R2:xx` diagnostics line (`uvf_keypad_raw` → `stm32_fsmc_keypad_scan` raw) that shows each
  row's pressed columns independent of the matrix, so pressing a key gives its true `(row, column)`
  (`boards/md2017/board.c`, `docs/testing/md2017-bringup.md` 2).
- **Next step (waiting on owner):** the owner presses Menu, Back, P1, P2 (and ideally every key) and reports the `kbd`
  readout; then correct `uvf_key_matrix[row][column]` in `boards/md2017/board.c`.
- The factory TYT image (`TYT2017-UVCSV-V4.05.bin`) can be decrypted for cross-checking (clean-room: key derived from
  the file with `stockfw.derive_key(..., MDUV380_KEY_SHA256)`, no key material stored) — but the raw scan is the
  authoritative source. Do **not** use OpenMD2017/OpenGD77 (GPL) for this.
- Open also: PE12/PE13/PE15/PA13/PA15 etc. inputs of unknown function (shown on the diag page), GPS pins.

## 4. Operating note (OTAP)

Keying the admin radio with the USB programming cable attached can couple RF into the serial link and drop it (on
Windows a WriteFile "device does not recognize this command"). It is a cable/EMC effect, not a firmware fault — the
command still reaches the fleet. Ferrite on the cable, a short shielded cable, antenna away, and low TX power mitigate
it. A possible future option: force OTAP transmissions to low power regardless of the channel.

## 5. Build / test

- `make BOARD=md2017` → `build/md2017/rfoundry_md2017_codec.bin` (codec merged from `codecs/TYT2017-UVCSV-V4.05.bin`).
- `make BOARD=dm32` → `build/dm32/rfoundry_dm32.bin` (no codec unless `DM32_049...` is in `codecs/`).
- `cd tests/host && make run` — full host suite passes (OTAP + privacy bands on by default).
- Two separate GUIs: `rfoundry_gui.py` flashes; `rfoundry_bench_gui.py` tests a running radio (OTAP tab, keypad diag).
