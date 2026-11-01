<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# RT-880 private UI bring-up

This is an experimental RadioFoundry target, not a working radio port. It compiles
the existing core, UI, input, settings, storage and radio state machine with a new
AT32F423 platform and board HAL. It has not run on a physical radio. The owner
explicitly waived clean-room role separation for this closed experiment on
November 1, 2026. No reference firmware code or tables are included.

The standalone entry point deliberately has no `board.mk` or registry entry:
release builders and flashers must not offer this unverified target. Run from the
repository root with the pinned Arm GCC toolchain and FreeRTOS submodule installed:

```sh
make -f boards/rt880/Makefile -j8
make -f boards/rt880/Makefile test stack-check
make -f boards/rt880/Makefile image
```

The explicit `image` target creates
`build/rt880-lab/rfoundry_rt880_ui_UNTESTED.bin`. No build command opens a serial
port or flashes hardware. The candidate load address is `0x08002800`; this is a
link assumption, not a verified permission to write or erase there.

## Implemented scope

- Cortex-M4F startup, vector relocation, inherited interrupt masking, internal
  8 MHz clock selection, DWT time base and FreeRTOS integration.
- Serial GPIO backend for the existing ST7789 driver. A 160 by 120, 4-bit frame
  buffer is doubled to landscape 320 by 240. Controller, orientation and electrical
  behavior need physical confirmation. The backlight is binary on/off.
- Raw 4 by 4 key matrix. Only raw positions 0, 1, 2 and 3 temporarily mean up,
  down, select and back. These are diagnostic assignments, not identified legends.
- Board diagnostics report the raw matrix mask and the restricted scope.
- Both TX and RX callbacks reject every request. All advertised bands are RX-only,
  PTT is not bound, and no RF registers are written. There is no audio output.
- External flash initialization/read/write/erase all fail; no storage area is
  allocated. Settings are volatile. Battery, ADC side keys, LEDs and power latch
  are not driven; battery readings are unknown (zero), not calibrated measurements.

`board_rf_safe()` only deasserts both observed RF chip selects. It does **not**
prove that an external PA is off. No external PA supply or bias pin is guessed.
Confirm physical transmitter inhibition, MCU identity, memory capacity and the
loader/recovery route before treating this image as suitable for flashing.
The owner's serial alone cannot establish those facts. See the
[hardware investigation](../../docs/spec/hardware-rt880.md) and
[bring-up plan](../../docs/testing/rt880-bringup.md).

## Validation

The host board test checks that RX/TX and storage requests cause no GPIO writes,
all band entries prohibit TX, no key binding requests PTT, all sixteen matrix
positions and idle scan correctly, and serial LCD transactions preserve chip
select, RGB565 byte order and the 2x row scale. This is a simulated pin interface.

The build has zero compiler/linker warnings and passes the static task-stack check.
The first linked image uses about 92 KB flash and 44 KB SRAM, including a 12 KiB
RTOS heap and 4 KiB reserved for C heap/main/interrupt stack. A CPU model reached
the first-task handoff and then startup housekeeping with 1,608 bytes of RTOS heap
free after task creation. The model bypassed pixel bursts and delays; it does not
validate interrupt delivery, actual context switching, clock accuracy or hardware.
Runtime allocation peaks and physical stack usage still need measurement.

The register definitions were checked against ArteryTek's official
[AT32F423 CRM header](https://github.com/ArteryTek/AT32F423_Firmware_Library/blob/master/libraries/drivers/inc/at32f423_crm.h)
and [CMSIS driver](https://github.com/ArteryTek/AT32F423_CMSIS_Driver).
No STM32 peripheral HAL is linked. FreeRTOS and newlib license notices apply to
binary distribution; retain the repository's `LICENSES` directory with source packages.
