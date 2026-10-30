# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
# TYT MD-2017 / Retevis RT82: STM32F405VG + HR_C6000 + two AT1846S (receiver A transceives, receiver B receives only),
# the UV380 board family with its own pins (UVF_MD2017: boards/uv380_family/pins.h; docs/spec/hardware-md2017.md).
# Bring-up: receiver A alone, transmit off unless MD2017_DEFS="-DMD2017_TX=1"
PLATFORM   := stm32f4
MCU        := stm32f405
# The 8 MHz reference; the image starts at sector 3, where the bootloader starts it (the MD-UV380's, hardware-mduv380.md 11)
HSE_VALUE  := 8000000U
APP_BASE   := 0x0800C000
MD2017_DEFS ?=
MCU_DEFS   += -DUVF_MD2017=1 $(MD2017_DEFS)
BOARD_DIRS := boards/md2017 boards/uv380_family platform/stm32f4/hrc platform/stm32f4/spi_flash platform/stm32f4/fsmc_keypad \
	drivers/rf/at1846s drivers/rf/subaudio drivers/modem/hrc6000 drivers/modem/hrc_common drivers/display/st7735 \
	drivers/flash/spi_nor drivers/input/quadrature
# The HAL's I2C driver besides the platform's modules
BOARD_HAL_MODULES := hal_i2c
# The 160x128 panel in 16 colors, every row visible (as the MD-UV380, unconfirmed on this radio)
MCU_DEFS   += -DGFX_INDEXED=1 -DGFX_FB_BYTES=10240u -DGFX_MAX_ROWS=128u -DUVF_LCD_HIDDEN_ROWS=0
MCU_DEFS   += -DUVF_HSE_XTAL_FIRST=1
