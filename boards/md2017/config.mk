# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
# MD-2017 feature defaults (config/features.mk lists them all): the MD-UV380's (boards/mduv380/config.mk), 720 KB in
# sectors 3-9, the codec in sectors 10 and 11 (boards/md2017/memory.ld)
CONFIG_DISPLAY_INSET ?= 0
CONFIG_DISPLAY_INSET_TOP ?= 1
CONFIG_DISPLAY_INSET_LEFT ?= 2
CONFIG_APRS_IGATE ?= n
MAXIMAL_VARS := LTO=1
MAXIMAL_SKIP := NET_PPP APRS_IGATE
BOARD_UNSUPPORTED += FONT_XL	# the 160x128 panel is too small for the large font
CONFIG_AM_RX ?= n
BOARD_UNSUPPORTED += AM_RX	# no air band receiver: both AT1846S are FM only (docs/spec/airband-and-broadcast.md)
CONFIG_BCAST_FM ?= n
BOARD_UNSUPPORTED += BCAST_FM	# no broadcast receiver
BOARD_UNSUPPORTED += NAND_STORAGE	# the data flash is SPI NOR
BOARD_UNSUPPORTED += VIBRATE	# no vibration motor
BOARD_UNSUPPORTED += FAULT_TRACE	# needs a host UART: the host port is USB
CONFIG_GPS ?= n
BOARD_UNSUPPORTED += GPS	# the GPS pins of the G version aren't mapped yet (hardware-md2017.md 3)
