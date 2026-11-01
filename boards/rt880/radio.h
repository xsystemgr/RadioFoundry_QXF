// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
#ifndef RT880_RADIO_H
#define RT880_RADIO_H
#include "hal_radio.h"
extern const hal_radio_t rt880_radio;
bool rt880_audio_test_boot_requested(void);
void rt880_rf_halt(void);
bool rt880_rf_diag(uint8_t line, char *buf, uint8_t len);
#endif
