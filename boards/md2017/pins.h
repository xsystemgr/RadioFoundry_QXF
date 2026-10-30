// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// TYT MD-2017 pin map: the UV380 family's with UVF_MD2017 (board.mk), and the inputs not yet known (hardware-md2017.md 3.1)
#ifndef MD2017_PINS_H
#define MD2017_PINS_H

#include "../uv380_family/pins.h"

// Read by the TYT image, meaning unknown (?): shown on the diagnostics page for the bench
#define PIN_IN_PA13		GPIOA, GPIO_PIN_13	// SWDIO, read as it is (not reconfigured)
#define PIN_IN_PA15		GPIOA, GPIO_PIN_15	// JTDI, read as it is
#define PIN_IN_PB0		GPIOB, GPIO_PIN_0
#define PIN_IN_PB1		GPIOB, GPIO_PIN_1
#define PIN_IN_PB10		GPIOB, GPIO_PIN_10	// pull-up
#define PIN_IN_PD10		GPIOD, GPIO_PIN_10
#define PIN_IN_PD11		GPIOD, GPIO_PIN_11
#define PIN_IN_PE13		GPIOE, GPIO_PIN_13	// pull-up
#define PIN_IN_PE15		GPIOE, GPIO_PIN_15	// pull-up

#endif
