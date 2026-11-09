// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The on-air FSK framing GOGUFW wraps a GGM2 packet in, for the BK4819 FSK FIFO. 50 words (100 bytes):
//   word[0]      = 0xABCD (sync head)
//   word[1..47]  = the 94-byte GGM2 packet, little-endian packed (47 words)
//   word[48]     = CRC-CCITT over the 94 packet bytes (a frame check on top of the packet's own CRC)
//   word[49]     = 0xDCBA (sync tail)
// This must match GOGUFW byte-for-byte so a RadioFoundry radio and a UV-K1/K5 exchange frames over the air.
#ifndef MSG_FSK_H
#define MSG_FSK_H

#include <stdint.h>
#include <stdbool.h>
#include "messenger/msg_packet.h"

#define MSG_FSK_WORDS		50u	// BK4819 FSK FIFO words the messenger sends (REG_5D = 0x6300, 100 bytes)
#define MSG_FSK_SYNC_HEAD	0xABCDu
#define MSG_FSK_SYNC_TAIL	0xDCBAu

// Pack a 94-byte GGM2 packet into the 50 FSK words sent to the BK4819 FIFO.
void msg_fsk_pack(uint16_t words[MSG_FSK_WORDS], const uint8_t packet[MSG_PKT_WIRE_LEN]);

// Reverse: check the sync head/tail and the frame CRC, then extract the 94 packet bytes. False on any mismatch.
bool msg_fsk_unpack(uint8_t packet[MSG_PKT_WIRE_LEN], const uint16_t words[MSG_FSK_WORDS]);

#endif
