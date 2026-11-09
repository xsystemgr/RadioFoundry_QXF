// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
#ifndef RT880_RADIO_H
#define RT880_RADIO_H
#include "hal_radio.h"
extern const hal_radio_t rt880_radio;
void rt880_rf_halt(void);
bool rt880_rf_diag(uint8_t line, char *buf, uint8_t len);
void rt880_bcast_speaker(bool on);	// the Si4732 broadcast receiver claims/releases the shared speaker gate
#if CONFIG_MESSENGER
#include "messenger/msg_packet.h"
void rt880_msg_set_call(const char *call);	// set the local callsign used as the GGM2 "from"
void rt880_msg_rx_arm(void);			// arm BK4819 FSK receive on the channel receiver
void rt880_msg_service(void);			// drain the FSK FIFO (called from the radio status poll, ~25 ms)
bool rt880_msg_poll(void);			// true when a new GGM2 message decoded (get it with rt880_msg_last)
bool rt880_msg_last(msg_packet_t *out);		// the most recent decoded message
bool rt880_msg_send(const char *text);		// send one GGM2 text frame on the current frequency, then return to RX
#endif
#endif
