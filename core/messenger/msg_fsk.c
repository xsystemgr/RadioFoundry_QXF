// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// GOGUFW-compatible BK4819 FSK framing of a GGM2 packet (msg_fsk.h).
#include "messenger/msg_fsk.h"

// 94 packet bytes -> 47 little-endian words (word[1..47]).
#define MSG_FSK_PKT_WORDS	(MSG_PKT_WIRE_LEN / 2u)		// 47

void msg_fsk_pack(uint16_t words[MSG_FSK_WORDS], const uint8_t packet[MSG_PKT_WIRE_LEN])
{
	words[0] = MSG_FSK_SYNC_HEAD;
	for (unsigned i = 0; i < MSG_FSK_PKT_WORDS; i++) {
		words[1u + i] = (uint16_t)packet[i * 2u] | ((uint16_t)packet[i * 2u + 1u] << 8);
	}
	words[1u + MSG_FSK_PKT_WORDS] = msg_packet_crc16(packet, MSG_PKT_WIRE_LEN);	// word[48]
	words[MSG_FSK_WORDS - 1u] = MSG_FSK_SYNC_TAIL;					// word[49]
}

bool msg_fsk_unpack(uint8_t packet[MSG_PKT_WIRE_LEN], const uint16_t words[MSG_FSK_WORDS])
{
	if (words[0] != MSG_FSK_SYNC_HEAD || words[MSG_FSK_WORDS - 1u] != MSG_FSK_SYNC_TAIL) {
		return false;
	}
	for (unsigned i = 0; i < MSG_FSK_PKT_WORDS; i++) {
		packet[i * 2u] = (uint8_t)(words[1u + i] & 0xFFu);
		packet[i * 2u + 1u] = (uint8_t)(words[1u + i] >> 8);
	}
	return msg_packet_crc16(packet, MSG_PKT_WIRE_LEN) == words[1u + MSG_FSK_PKT_WORDS];
}
