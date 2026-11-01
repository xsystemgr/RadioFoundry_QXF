<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# Radtel RT-880: port investigation

Status: research only. There is no buildable board target or tested RadioFoundry image yet.
The first target is analog FM receive. Digital voice is a separate feasibility question.
The owner's exact model suffix and PCB revision still need confirmation.

## Evidence and provenance

The owner supplied `nicfw880_V5.14.01.bin` for offline inspection on November 1, 2026.
It is an aftermarket application image, not a full device backup. Its identity is recorded here
so future observations can be tied to the same input. The binary and disassembly stay outside
this repository. This page records factual observations, not another firmware's implementation,
initialization tables, strings, menus, fonts, or function structure. See [Clean-room rules](../CLEANROOM.md).

| Observation | Value | Qualification |
|---|---|---|
| Image length | 162,416 bytes (`0x27A70`) | Measured from the supplied file |
| SHA-256 | `01aed24fb6c04f96f801e3d01a1d423d415e9551c860a5ec848e7fce50a6ad03` | Identity only; no comparison with an independently downloaded release |
| Candidate application address | `0x08002800` | Strong static evidence; not a verified flash permission or erase boundary |
| End at that address, exclusive | `0x0802A270` | Application address plus image length, not total flash capacity |
| Initial stack pointer | `0x20009A68` | First vector word; does not establish RAM capacity |
| Reset vector | `0x080029F5` | Thumb entry address is `0x080029F4` |
| NMI vector | `0x080029FD` | Entry address is `0x080029FC` |
| HardFault vector | `0x080029FF` | Entry address is `0x080029FE` |
| Instruction family | Arm Thumb / Cortex-M style | Exact MCU, core revision and memory sizes remain unverified |

The candidate application address was checked against the startup and exception vector targets
in the external investigation. The image does not contain the bytes below that address.
Do not turn the candidate address into a linker script or flasher profile until device identity,
bootloader behavior, flash geometry and protected regions have independent confirmation.

The generic [image header inspector](../../tools/inspect_image_header.py) reproduces the size,
hash and vector observations without decoding instructions:

```sh
python3 tools/inspect_image_header.py /path/to/nicfw880_V5.14.01.bin --base 0x08002800
```

The tool validates vector plausibility at a supplied address. It does not infer the address,
authenticate the image, identify the MCU, inspect a connected radio, or approve flashing.

## Hardware questions to resolve

| Part | Current evidence | Required confirmation |
|---|---|---|
| MCU | Cortex-M style application image | Readable package marking, board revision, exact datasheet, flash/RAM/erase geometry |
| RF transceiver | Not identified by this specification | Package marking, number of chips, bus wiring, public register documentation |
| Broadcast/shortwave receiver | Aftermarket software advertises tuner functions | Fitted receiver marking, bus, clocks, audio routing and variant differences |
| Display | Not established | Controller, geometry, interface, reset/backlight pins, voltage |
| Keypad and controls | Not established | Matrix wiring, encoder if fitted, PTT and programming-jack interaction |
| External storage | Not established | Part marking, JEDEC identity, bus, calibration and codeplug locations |
| GPS | Variant-dependent presence not verified | Exact model suffix, module marking, UART, baud rate and power control |
| Power and audio | Not established | Power latch, PA enable, RX/TX switching, amplifier, ADC/DAC paths, battery divider |
| Loader | Published updater instructions exist | Protocol, actual application destination, erase scope and recovery on this revision |

Software feature labels are not chip identification. A supported feature in aftermarket firmware
does not establish an audio path, register sequence or hardware capability for this port.

## RadioFoundry integration

The existing [board interface](../../hal/board.h) permits analog-only boards: `dmr` may be NULL
and `CAP_DMR` is optional. The first implementation should use that boundary rather than
fabricating DMR or vocoder interfaces.

| Existing component | Potential reuse | Condition |
|---|---|---|
| `core/radio`, `core/ui`, settings and codeplug | Portable behavior and UI | Sufficient memory and working board HAL |
| `drivers/rf/bk4819` | RF register driver | Only if the fitted chip and supported operating path match |
| `drivers/rf/fd6818` | Alternative RF driver | Only if that part is independently identified |
| `drivers/` display and input drivers | Existing chip implementations | Controller, bus and electrical wiring must match |
| Existing GPS, APRS and AX.25 code | Protocol and signal processing | Confirmed GPS/UART and sampled audio or tone paths, with measured CPU/RAM budget |
| `platform/stm32f4` | Possible platform reference | A similar peripheral address map is insufficient; establish actual silicon compatibility first |

There is no Si4732 driver in the inspected tree at commit
`090c53138ba961cbb841e478d7faeb517708d874`. If this part is confirmed, implement a chip driver
from public documentation and a clearly permissive dependency, then connect it through the
appropriate HAL. A broader AM/SSB tuner may need a generic HAL extension beyond broadcast FM.

Do not register `boards/rt880/board.toml` until its platform, memory map and minimum board
interfaces are concrete. A speculative registry entry would advertise an image that cannot be built.
The next steps and acceptance evidence are in [Bring-up plan](../testing/rt880-bringup.md).

## External identification references

- [Author's release notes](https://www.patreon.com/nicsure/posts/nicfw880-v5-14-149784888)
- [Author's flashing instructions](https://github.com/nicsure/nicFW880_950_Docs/blob/main/RMS880.wiki/Firmware-Tab.md)

These identify the reference image and updater context. They do not license incorporating
the firmware into RadioFoundry or establish this unit's hardware revision.
