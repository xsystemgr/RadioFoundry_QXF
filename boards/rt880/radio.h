// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
#ifndef RT880_RADIO_H
#define RT880_RADIO_H
#include "hal_radio.h"
extern const hal_radio_t rt880_radio;
void rt880_rf_halt(void);
bool rt880_rf_diag(uint8_t line, char *buf, uint8_t len);
void rt880_bcast_speaker(bool on);	// the Si4732 broadcast receiver claims/releases the shared speaker gate
#endif
