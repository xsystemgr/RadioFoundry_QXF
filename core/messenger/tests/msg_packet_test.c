// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Host test for the GGM2 messenger packet: wire layout, CRC-CCITT known answer, round-trip and GOGUFW compatibility.
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "messenger/msg_packet.h"
#include "messenger/msg_fsk.h"

static void test_fsk(void)
{
	uint8_t pkt[MSG_PKT_WIRE_LEN];
	assert(msg_packet_build_text(pkt, sizeof pkt, 0x1234u, "SV4QXF", "Hi", 2) == MSG_PKT_WIRE_LEN);

	uint16_t words[MSG_FSK_WORDS];
	msg_fsk_pack(words, pkt);
	// GOGUFW on-air layout: sync head/tail and little-endian packed packet.
	assert(words[0] == 0xABCDu);
	assert(words[MSG_FSK_WORDS - 1] == 0xDCBAu);
	assert(words[1] == ((uint16_t)pkt[0] | ((uint16_t)pkt[1] << 8)));	// 'G' | 'G'<<8
	assert(words[1] == (uint16_t)('G' | ('G' << 8)));
	assert(words[48] == msg_packet_crc16(pkt, MSG_PKT_WIRE_LEN));		// frame CRC word

	// Round-trip: unpack recovers the exact packet and verifies the frame CRC.
	uint8_t out[MSG_PKT_WIRE_LEN];
	assert(msg_fsk_unpack(out, words));
	assert(memcmp(out, pkt, MSG_PKT_WIRE_LEN) == 0);

	// A corrupted word fails the frame CRC; a bad sync fails too.
	words[5] ^= 0x0100u;
	assert(!msg_fsk_unpack(out, words));
	words[5] ^= 0x0100u;
	words[0] = 0;
	assert(!msg_fsk_unpack(out, words));
}

int main(void)
{
	// CRC-CCITT (0x1021, init 0xFFFF) known answer: "123456789" -> 0x29B1. This is the exact algorithm GOGUFW uses,
	// so a match guarantees frame-check interoperability.
	assert(msg_packet_crc16((const uint8_t *)"123456789", 9) == 0x29B1u);

	uint8_t buf[MSG_PKT_WIRE_LEN];
	assert(msg_packet_build_text(buf, sizeof buf, 0xBEEFu, "SV4QXF", "Hi", 3) == MSG_PKT_WIRE_LEN);

	// Exact GGM2 wire offsets (must match the GOGUFW firmware byte-for-byte).
	assert(buf[0] == 'G' && buf[1] == 'G' && buf[2] == 'M' && buf[3] == '2');
	assert(buf[4] == 2);			// version
	assert(buf[5] == MSG_PKT_TYPE_TEXT);
	assert(buf[6] == 0);			// flags
	assert(buf[7] == 0xEF && buf[8] == 0xBE);	// id, little-endian
	assert(buf[9] == 3 && buf[10] == 3);	// ttl_init / ttl_remain
	assert(memcmp(&buf[11], "SV4QXF\0\0", 8) == 0);	// from, zero-padded to 8
	assert(memcmp(&buf[19], "ALL\0\0\0\0\0", 8) == 0);	// to = ALL (broadcast)
	assert(buf[27] == 2);			// payload_len
	assert(buf[28] == 'H' && buf[29] == 'i');
	// CRC over [0..91], little-endian at [92..93]
	uint16_t crc = msg_packet_crc16(buf, MSG_PKT_WIRE_LEN - 2u);
	assert(buf[92] == (uint8_t)(crc & 0xFF) && buf[93] == (uint8_t)(crc >> 8));

	// Round-trip parse.
	msg_packet_t p;
	assert(msg_packet_parse(buf, sizeof buf, &p));
	assert(p.type == MSG_PKT_TYPE_TEXT && p.id == 0xBEEFu && p.ttl_init == 3);
	assert(strcmp(p.from, "SV4QXF") == 0 && strcmp(p.to, "ALL") == 0);
	assert(p.payload_len == 2 && memcmp(p.payload, "Hi", 2) == 0);

	// A corrupted byte must fail the CRC check.
	buf[30] ^= 0x80u;
	assert(!msg_packet_parse(buf, sizeof buf, &p));

	// Max-length payload (64 bytes) still fits in one 94-byte frame.
	char big[80];
	memset(big, 'A', 64);
	big[64] = 0;
	assert(msg_packet_build_text(buf, sizeof buf, 1, "A", big, 1) == MSG_PKT_WIRE_LEN);
	assert(msg_packet_parse(buf, sizeof buf, &p) && p.payload_len == 64);

	// Control frames.
	assert(msg_packet_build_ping(buf, sizeof buf, 7, "A") == MSG_PKT_WIRE_LEN);
	assert(msg_packet_parse(buf, sizeof buf, &p) && p.type == MSG_PKT_TYPE_PING);

	assert(msg_packet_selftest());

	test_fsk();

	printf("msg test: OK (GGM2 packet + FSK framing, CRC-CCITT KAT, round-trip, tamper, GOGUFW-compatible)\n");
	return 0;
}
