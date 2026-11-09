// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Board-provided radio hooks for the analog messenger UI. The core UI (core/ui/ui_msgr.c) calls these; the active board
// implements them over its BK4819 FSK modem (e.g. boards/rt880/radio.c). This keeps the UI board-agnostic - it never
// includes a board header. Only built when CONFIG_MESSENGER is set.
#ifndef MSG_RADIO_H
#define MSG_RADIO_H

#include <stdbool.h>

void msg_radio_set_call(const char *call);	// set the local callsign used as the GGM2 "from"
void msg_radio_rx_arm(void);			// start / keep FSK receive armed (decoded frames go to the store)
bool msg_radio_poll(void);			// true once per newly decoded frame (UI refresh edge)
bool msg_radio_send(const char *text);		// transmit one GGM2 text frame; stores a Sent copy on success

#endif
