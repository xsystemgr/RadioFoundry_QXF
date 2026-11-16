<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# NXDN48 stack (vendored)

The portable NXDN48 protocol + software-4FSK-modem stack, being ported to the **MD-2017** (HR_C6000) as the first
step of native NXDN support.

## Provenance and license

**These files are GPL-3.0-or-later**, vendored from
[donttellmeimcute/DM32-nxdn-p25](https://github.com/donttellmeimcute/DM32-nxdn-p25) (maintainer *lyra XE1POC*), itself
a derivation of [Open25-DM32](https://github.com/tylerwatt12/Open25-DM32) (tylerwatt12), both built as patches on
[RadioFoundry](https://github.com/RadioFoundry/RadioFoundry). The original per-file `SPDX-License-Identifier`
headers are kept. The full license text is in [`../../LICENSES/GPL-3.0-or-later.txt`](../../LICENSES/GPL-3.0-or-later.txt).

> **Licensing note:** linking this GPL-3.0 code into the MD-2017 firmware makes that firmware image GPL-3.0-or-later.
> The rest of the RadioFoundry tree is MIT, which GPLv3 can incorporate; keep the GPL notices and ship the source.

The AMBE/IMBE voice codec is **not** here and is not distributed — it is supplied per radio (the same model RadioFoundry
and the upstream projects use).

## What's portable vs. what's board work

| File | Role | Portable? |
|---|---|---|
| `nxdn.c/.h` | Frame build/parse, FEC, PN9 scrambler, FSW | ✅ hardware-agnostic |
| `nxdn_call.c/.h` | Call state, LICH/SACCH/FACCH, 72-bit AMBE+2 air frames | ✅ |
| `nxdn_link.c/.h` | Symbol/dibit stream, preamble/tail, TX ring ↔ vocoder pacing | ✅ |
| `nxdn_modem.c/.h` + `nxdn_modem_tables.inc` | **Software** 4FSK modem: 2400 sym/s at 8 kHz, RRC pulse shaping, Gardner timing | ✅ integer DSP |

NXDN48 is a **software** 4FSK modem (±1050/±350 Hz, 2400 sym/s) that runs over the radio's **FM audio path** — not the
chip's native DMR modem. The MD-2017-specific glue still to do:

- **RX** (tractable): feed the HR_C6000 FM-RX discriminator audio (8 kHz, 16-bit — the FM RX RAM buffers) into
  `nxdn_demod_samples()`, then `nxdn_link`/`nxdn_call` → the AMBE vocoder → speaker.
- **TX** (open RF-RE): make `nxdn_mod` samples the HR_C6000's FM **modulation source**. Per `docs/spec/modem-hrc6000.md`
  §8.1 this is **not yet established on our hardware** (the FM data RAM path exists in the manual but no setting has made
  MCU audio the modulation source). This is the same unsolved problem as FM-data/APRS TX on the MD-2017, plus deviation
  calibration (the upstream NXDN TX deviation is itself still being dialed in).

## Host tests (pass)

```sh
cc -std=c11 -Wall -Wextra -O2 -Icore/nxdn \
  core/nxdn/tests/test_nxdn_call.c core/nxdn/nxdn_call.c core/nxdn/nxdn.c -lm -o /tmp/t_call && /tmp/t_call
cc -std=c11 -Wall -Wextra -O2 -Icore/nxdn \
  core/nxdn/tests/test_nxdn_link.c core/nxdn/nxdn_link.c core/nxdn/nxdn.c core/nxdn/nxdn_call.c \
  core/nxdn/nxdn_modem.c -lm -o /tmp/t_link && /tmp/t_link
```

Both the protocol/call path and the full TX↔RX software-modem round-trip pass on the host, so the portable foundation
is verified in this tree. The remaining work is the HR_C6000 board glue above.
