/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 The RadioFoundry Authors */
/* Candidate device layout only; loader erase geometry has not been verified. */
ENTRY(Reset_Handler)
MEMORY {
 FLASH (rx) : ORIGIN = 0x08002800, LENGTH = 0x3D800
 RAM (xrw) : ORIGIN = 0x20000000, LENGTH = 48K - 8
}
_estack = ORIGIN(RAM) + LENGTH(RAM);
SECTIONS {
 .isr_vector : { _vectors = .; KEEP(*(.isr_vector)) } > FLASH
 .text : { *(.text*) *(.rodata*) . = ALIGN(4); } > FLASH
 .ARM.extab : { *(.ARM.extab*) } > FLASH
 .ARM.exidx : { __exidx_start = .; *(.ARM.exidx*) __exidx_end = .; } > FLASH
 _sidata = LOADADDR(.data);
 .data : { . = ALIGN(8); _sdata = .; *(.data*) . = ALIGN(4); _edata = .; } > RAM AT > FLASH
 .bss (NOLOAD) : { . = ALIGN(8); _sbss = .; *(.bss*) *(COMMON) . = ALIGN(8); _ebss = .; } > RAM
 .reserved (NOLOAD) : { _end = .; end = .; . += 4096; } > RAM
 ASSERT(_ebss + 4096 <= _estack, "SRAM must retain 4 KiB for C heap and main/interrupt stack")
}
