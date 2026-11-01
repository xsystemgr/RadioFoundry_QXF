<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# Radtel RT-880: port investigation

Status: fix2 orientation appears correct on the owner unit. Response remains slow; signal indication reacts but the speaker is silent. Fix3 clock/volume diagnostics await testing.
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
The private lab linker uses this candidate address as an explicit assumption. This does not
verify device identity, bootloader behavior, flash geometry or protected regions, and it must
not become a flasher profile until those facts have independent confirmation.

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


## Software-observed GPIO assignments

Updated November 1, 2026. The external reference investigation recovered the initialized data
and tested known byte transfers in an isolated CPU emulator. The following are software-observed
assignments for the reference SHA-256 above, not measured board wiring. No binary, disassembly,
register initialization sequence or external implementation is included here.

| Interface | Clock | Select | Data | Other |
|---|---|---|---|---|
| RF path A | PC8 | PC7, active low | PC6, bidirectional | Board initialization selects this path |
| RF path B | PF6 | PA8, active low | PC9, bidirectional | Software can select this separate pin group |
| External SPI NOR | PB13 | PB12, active low | PB15 MOSI; PB14 MISO | Chip part and capacity not identified |
| Serial display | PB3 | PA15, active low | PB5 output | PC13 D/C, high for data; PC14 likely reset, active low |
| Tuner I2C | PC0 SCL | Address 0x11 | PC1 SDA, bidirectional | Command structure consistent with Si47xx |

The display initialization addresses a 240 by 320 pixel window with RGB565 format and a
ST77xx-compatible command set. ST7789-class is a candidate; controller ID has not been read.
The physical number and role of RF chips cannot be established from two selectable pin groups alone.
USART6 is initially configured at 38400 baud with PA11/PA12 alternate function 8. Its wiring,
TX/RX assignment and purpose remain unconfirmed.

Five emulator checks reconstructed the expected output bytes for both RF paths, external SPI
and display command/data. This does not verify clock timing, slave responses or electrical levels.
PTT, power latch, PA control, RF switching, audio routing and calibration boundaries remain open.
These observations narrow the investigation; they do not authorize a flash image or transmitter bring-up.


## Input and clock observations

The keypad is a 4 by 4 matrix. Drive outputs PB10, PB2, PB1 and PB0 in that order;
read inputs PC10, PC11, PC12 and PD2. An asserted input is high while the selected
output is high. Return all driven outputs low between scans. Sixteen synthetic
single-key positions plus idle were checked in an isolated CPU emulator. The physical
legends and the new RadioFoundry action map have not been established.

PB6 is an active-low request input, strongly identified as PTT by its use in the
transmit-state path. A separate remote request can also assert the request. All four
physical/remote combinations were checked in emulation. This does not establish whether
the programming cable can influence the input.

Additional keys use ADC channel argument 12. Stable sample bands were observed at
1125-1659, 1660-2771 and 2772-3499 counts, with no key below 1125 or at/above 3500.
Ten boundary cases were checked. The physical ADC pin and key labels remain unconfirmed.

PA9 controls display backlight/enable alongside display sleep/on commands. PC4 and PF8
change in audio/tone/squelch paths; their external circuitry is unconfirmed. Do not label
these pins as PA control merely because they change during radio operation.

The reference clock setup computes SYSCLK/AHB/APB2 at 150 MHz and APB1 at 75 MHz
in an emulator whose ready bits follow enables and source status follows selection.
These are software-computed clocks, not hardware measurements or a copied startup recipe.

## RadioFoundry implementation boundary

Use the existing core and HAL, a new platform and board glue, and existing chip drivers
where their documented hardware matches. In particular, the existing display driver has
command/data/pixel bus callbacks; a serial GPIO backend does not require replacing the UI.
The existing key matrix decoder can consume normalized asserted-column bits.

A full 240 by 320 RGB565 frame needs 153,600 bytes. A 4-bit indexed frame needs
38,400 bytes and leaves only 10,752 bytes of a 48 KiB SRAM for all other runtime use.
Choose a measured display geometry/scale and feature profile after a linker map exists.
Do not claim the default configuration of an existing board fits this MCU.

A private FM receive board implementation now builds through
`make -f boards/rt880/Makefile image` (run from the repository root). It uses the
existing RadioFoundry core and a new platform/HAL; its physical reception has not been verified.
The owner explicitly waived clean-room role separation for this closed experiment
on November 1, 2026. No reference firmware code or initialization tables were imported.
The remaining hardware blockers are external PA/power control, MCU verification,
storage protection and a verified loader/recovery route. See the
[experimental target](../../boards/rt880/README.md) for restrictions and tests.


## Band routing and loader investigation

The routing helper selects the second RF bus while updating register 0x33, then restores
the prior bus selection. Twenty-six frequency/mode cases were checked in an isolated
emulator. PF0 is high above zero through 70 MHz and low otherwise. PC15 is high only
in the transmit route above zero through 70 MHz. In that low-band transmit route,
PB11 is low through 39.5 MHz and high above it. Other routes leave PB11 unchanged in
this helper. These are software-observed levels, not identification of the external
PA transistor, bias supply or RF switch. A board-wide safe-off implementation is still open.

The author's web and desktop flashers agree on 115200 8N1 and 1024-byte data payloads.
Each packet has an 8-bit additive checksum seeded with 0x52 for the RT-880 profile,
0x00 for the UV-98 profile. The host sends the initial control twice and then the update
control, expecting an ACK after each. The update control's RT-880 checksum is 0x18,
which conflicts with an older external protocol note. Data packets contain the low
16 bits of the file byte offset, which wraps every 64 KiB. The checked hosts do not
send the end control described in that older note.

The host source does not establish the device's erase boundaries, bootloader survival
or readback verification. Do not send erase-stage controls as an identification probe.
The author's RMS documentation says units with serial numbers below 300 use the
UV-98 profile, even if branded RT-880. Confirm the unit before selecting a profile.

Application recovery through the PTT-at-power-on loader is documented but untested on
the owner's unit. Full external storage restore through RMS requires a running nicFW
application. Retain both a matching working application image and device-specific
storage/codeplug/power/calibration backups; the application file alone contains none
of those external data backups. No radio was written during this investigation.

## Memory capacity experiment

The owner supplied serial `25UV98P00696`. Its suffix alone does not verify the MCU,
loader profile or PCB revision. The following budget remains conditional on a 48 KiB
SRAM device. No hardware identification was performed.

An existing RadioFoundry board configuration was compiled with Arm GCC 13.3.Rel1,
LTO and a 160 by 120 indexed frame buffer. This was a capacity experiment using
the DM-1701 platform and drivers, not an RT-880 target. FM, VFO, FM VFO, diagnostics,
UI color/settings, key assignment, channel list and scan were enabled. All other
feature switches were explicitly disabled, including DMR, USB, GPS and APRS.

| Allocation | Bytes |
|---|---:|
| Initialized RAM data | 1,088 |
| Main zero-initialized state, including 9,600-byte frame buffer | 27,520 |
| CPU-only zero-initialized state | 720 |
| RTOS heap, reduced from 28 KiB for this experiment | 12,288 |
| C heap and main/interrupt stack reservations | 6,144 |
| Total allocated RAM | 47,760 |
| Remaining in 48 KiB | 1,392 |
| Loaded application flash | 112,456 |

The default 28 KiB RTOS heap instead requires 64,144 bytes of aggregate RAM,
exceeding 48 KiB by 14,992 bytes. A second link placed both CPU-only sections
and ordinary RAM sections in one 48 KiB region and succeeded with the reduced
heap. It retained the reference board's image address and startup; it is not
compatible firmware for the RT-880. No BIN was generated for distribution.

The reference configuration's static task-stack check passed. This does not
validate runtime heap peaks, interrupt nesting, the new platform's call chains
or safe boot. The 12 KiB heap needs measured allocation peaks and failure-path
testing. Do not shrink task stacks merely to obtain a successful link.

The generic [ELF memory inspector](../../tools/inspect_elf_memory.py) counts all
allocated sections inside explicitly supplied RAM regions, including NOLOAD
reservations. It excludes writable-tagged flash constructor arrays and counts
read-only code/data if linked in RAM. It checks aggregate capacity, not address
compatibility, section alignment gaps, hardware identity or flashing safety.

This establishes a plausible reduced-memory starting point for an independently
implemented platform. It does not complete board glue, the PA safe-off state,
display adaptation, calibration protection or loader/recovery validation.


## Private UI checkpoint (before receive integration)

The standalone target is intentionally absent from release and flashing registries.
The first linked image allocates 91,852 bytes in flash and 44,456 bytes in SRAM,
including 12 KiB RTOS heap and 4 KiB C heap/main stack reservation. The candidate
SRAM region ends eight bytes below 48 KiB; the linker leaves another 4,688 bytes
unallocated. This memory map is conditional on the assumed AT32F423 part.

The HAL rejects both RX and TX and all external storage operations. It deasserts
both RF chip selects, but it cannot establish external PA shutdown. It does not
bind PTT or configure unverified PA/battery/audio controls. The UI is landscape
160 by 120, scaled 2x through the existing ST7789 driver. Raw key positions 0-3
have temporary navigation assignments; physical legends are not established.

Host tests cover request rejection without GPIO writes, band/key TX restrictions,
all matrix positions, and LCD chip select, pixel byte order and scaling. A CPU
model reached kernel task handoff and startup housekeeping with 1,608 bytes of RTOS
heap free. Delays and pixel bursts were bypassed in that model. It does not prove
hardware compatibility, safe RF state, live interrupt/context switching or runtime
peak memory. No connected hardware was flashed or tested.


## Private FM receive target

The owner's unit currently runs nicFW 5.14.x (owner confirmation). An independent
report by the nicFW author identifies the RT-880 MCU as AT32F423, strengthening
that platform choice. The same author's schematic-based explanation says the
second BK4819 has no receive input path; it serves crossband transmission. This
is a different topology from a two-chip superheterodyne receiver. No public
schematic was obtained for the owner's exact PCB.

- [Author's MCU comparison](https://www.patreon.com/nicsure/posts/rt-950-pro-vs-rt-153711437)
- [Author's RF-path explanation](https://www.patreon.com/nicsure/posts/rt-880-is-very-133417003)

Further isolated basic-block observations confirm PF8 high plus PC4 low when
audio is opened, PC4 high at mute, and PF8 low in speaker shutdown. They establish
software-selected levels; physical amplifier function and isolation still need
measurement. VHF/UHF receive routing remains RF B register 0x33 = 0x10/0x20,
with RF B enables and PA level held at zero. MCU PF0 and PC15 remain low in this
restricted receive profile; no HF path is attempted.

The new HAL uses the existing RadioFoundry BK4819 register/bus, analog-mode,
bandwidth, tuning, AF and RSSI helpers. Its new receive-only initialization entry
point avoids the ordinary initialization's nonzero GPIO pulse. Both chip IDs
must match 0x4819, then RF A receives at 136-174 or 400-480 MHz FM. The existing
core supplies squelch and channel behavior. TX, sub-audio requests and storage
writes are refused. A held raw matrix position 4 supplies monitor; printed key
legends remain unidentified.

All six task-stack checks and host tests pass. A CPU/pin model runs startup and
60 core radio-task iterations, tunes 145.500 MHz, opens audio on modeled carrier
and closes it on loss, with no TX state, nonzero PA level or NOR select. This is
implementation/model evidence, not measured RF reception. The implementation,
restrictions and test entry points are in the
[experimental target](../../boards/rt880/README.md).


Artery's [ES0010 errata](https://arterytek.com/file/download/2228) describes PC13
remaining high-impedance after slow power-on. This pin is the display D/C line.
The platform now requests a software reset when the POR flag is set and the
software-reset flag is clear, before peripheral startup. It does not clear those
flags itself. The host test covers all four flag combinations, and the CPU model
covers one requested reset followed by a successful kernel handoff. Bootloader
execution and its reset-flag handling remain outside that model.


## Owner boot report and portrait correction

The owner first reported a black screen after flashing, then confirmed the RX
image boots and displays the UI after waiting. The report is slow drawing and
landscape orientation, not an established startup crash. The loader remains
accessible. RF IDs, audible reception and squelch behavior were not reported.

The new lab build uses a 120 by 160 indexed frame, 240 by 320 physical pixels at
2x scale, and MADCTL 0x20 as observed in the reference image. Pixel storage stays
9,600 bytes; row metadata grows by 160 bytes. The two per-bit DWT waits in the
LCD sender are removed at the fixed 8 MHz clock; RF bus waits are retained.
The modeled LCD byte cost drops from 559 to 135 instructions, but physical
refresh speed and the corrected orientation await the owner's next test.

## Fix2 after owner photo

The owner confirmed the photo is the portrait binary and reports only a slight
speed improvement. It shows a 90-degree rotation and noisy strip. Fix2 changes
MADCTL from 0x20 to 0x00 for the 240-column by 320-row window, clearing the axis
exchange bit. The host serial model checks exact first/last-row bounds.

The LCD byte sender combines clock-low/data writes and unrolls the bits. All
256 bytes pass the compiled GPIO model, with 52 instructions per byte versus
135 in the previous portrait build. The CPU remains at 8 MHz; RF timing and
PA/TX restrictions are unchanged. Actual responsiveness, orientation and RF
reception still require the owner's test. Earlier sections record prior builds.

## Fix3: slow response and silent speaker

The owner confirms fix2 orientation appears correct, reports continued slow
response and a signal indication when another radio transmits, with no speaker
audio. Monitor has not been tried. Actual demodulation remains unconfirmed.

Fix3 selects documented HICK48 with one flash read wait state, retaining
calibration and updating DWT time conversions and SystemCoreClock/RTOS timing.
It preserves the fix2 panel geometry and orientation. RF output mode is now
configured once per turnaround instead of every bit; RF delays remain intact.
A software Volume hook uses the existing BK4819 helper, starts at 50 percent,
and mutes at zero. The physical knob ADC is still unimplemented.

Diagnostics expose last AF/gain readbacks, commanded Audio OPEN/MUTE, volume and
RSSI/noise. These aid the next Squelch 0/known-FM-signal test; they are not proof
of amplifier output. Host tests and reset/inherited-PLL CPU models pass; owner
checks of speed, audio and RF remain outstanding. Earlier sections are history.

Clock source: [official AT32F423 RM 2.03](https://www.arterychip.com/download/RM/RM_AT32F423_EN_V2.03.pdf),
sections 4.1.1, 4.3.23, 4.3.24 and 5.8.1.

## Fix4: keypad translation

The owner reports the menu buttons do not match the front panel. Isolated
reference key-decoder observations map electrical bits 0..3 to 1/4/7/*,
4..7 to 2/5/8/0, 8..11 to 3/6/9/# and 12..15 to Menu/Up/Down/Exit. The
original positional key map was incorrect. Front labels are inferred from
numeric-entry/navigation consumers and the visible keycaps, awaiting an owner
check; no claim is made about ADC side-key identities.

Fix4 corrects board-level actions and arrow long/repeat actions. Green Menu
selects, orange Exit backs out, * opens Quick (held: lock) and # V/M toggles
VFO/channel. Monitor is available in Quick instead of on a digit key. The
actual input-engine host test checks every physical scan bit and held gestures.
The radio core, fix3 clock/audio path and RX-only restrictions are unchanged.

## Keys verified; silent audio investigation

The owner confirms fix4 keys work, but reports silence even with squelch off.
Isolated reference RX runs confirm AF 6140, receiver state BFF1, and DAC Gain
settings 0/7/15 -> B010/B1E7/B3FF. The previous PF8-high/PC4-low opening
observation remains valid. The current port requests the same AF/state/gates;
its volume changes only the 0x48 DAC nibble, not the full reference gain curve.

Direct ADC-reader calls identified so far are channel 12 (side keys) and 7
(battery). An ADC-backed physical volume knob has not been established; prior
wording suggesting one was premature. Analog knob wiring remains an inference.
Reference RX power 1F0F and filters 0000 differ from this port's 1D0F/C000, but
neither difference is a proven cause of silence. Physical IDs/AF/gain/audio
readbacks are required before attributing the fault to the external analog
path or another receiver setup field. No firmware change is made at this
checkpoint; the working keymap and current binary remain unchanged.

## Fix5 after owner diagnostic photo

The photo shows IDs 4819/4819, AF 6140, gain B3BF, Audio OPEN at V:100,
tune 433125000 Hz, configured CPU 48 MHz and RSSI/noise about -82/18. Speaker
silence remains unresolved. Fix5 adds an opt-in local RX Tone1 test: hold * at
startup to request two 150 ms 1 kHz pulses after the first valid receive tune.
AF selection 2 (6240) routes Tone1 to the receiver DAC; no TX APIs are used.
The test restores generator state/audio request and runs once per opted-in boot.

Diagnostics add GPIO pad input levels for PC4/PF8, registers 30/37 and voice
amplitude 64, plus the tone's AF/pad samples. Expected open pads are 0/1;
TestAF 6240/P:2 confirms only modeled or sampled logic, not audibility. In
Diagnostics, * pages to the additional rows. Volume is Settings > Sound >
Volume, maximum step 16, rather than Quick settings.

RX-only initialization now skips the shared startup word 0302, whose bit 1 is
TX DSP enable; PA and synthesizer were off at that word. Prior audit checked
bit 14 and was incomplete. The corrected tests reject enable bits 3:1 on every
RX-only frame. Ordinary main/LO initialization is unaffected. This correction
is not evidence for the cause of silence. The owner must test tone audibility.
