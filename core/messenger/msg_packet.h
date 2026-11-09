// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// GOGUFW-compatible analog text-messenger packet (the "GGM2" native frame), ported wire-for-wire from the GOGUFW
// UV-K1/UV-K5 firmware so a RadioFoundry radio with a BK4819 FSK modem can exchange messages with real GOGUFW radios.
// This is the application packet; the BK4819 FSK hardware wraps it with its own 0xABCD.. sync framing on the air.
//
// Wire layout, 94 bytes, little-endian, CRC-CCITT over bytes [0..91]:
//   [0..3]  magic 'G','G','M','2'        [4] version(2)   [5] type   [6] flags
//   [7..8]  id (u16 LE)                  [9] ttl_init     [10] ttl_remain
//   [11..18] from (8, zero-padded)       [19..26] to (8)
//   [27] payload_len                     [28..91] payload (<=64)     [92..93] CRC16 LE
#ifndef MSG_PACKET_H
#define MSG_PACKET_H

#include <stdint.h>
#include <stdbool.h>

#define MSG_PKT_MAGIC0		'G'
#define MSG_PKT_MAGIC1		'G'
#define MSG_PKT_MAGIC2		'M'
#define MSG_PKT_MAGIC3		'2'
#define MSG_PKT_VERSION		2u
#define MSG_PKT_TYPE_TEXT	1u
#define MSG_PKT_TYPE_ACK	2u
#define MSG_PKT_TYPE_PING	3u
#define MSG_PKT_TYPE_PONG	4u
#define MSG_PKT_TYPE_WAKE	5u
#define MSG_PKT_TO_ALL		"ALL"
#define MSG_CALLSIGN_LEN	8u	// on-wire callsign field width (zero padded)
#define MSG_TEXT_LEN		64u	// maximum payload bytes
#define MSG_PKT_WIRE_LEN	94u	// total on-wire length of a GGM2 packet
#define MSG_PKT_MAX_PAYLOAD	MSG_TEXT_LEN
#define MSG_PKT_DEFAULT_FROM	"UVK1"	// GOGUFW's default source when no callsign is set

typedef struct {
	uint8_t  type;
	uint8_t  flags;
	uint16_t id;
	uint8_t  ttl_init;
	uint8_t  ttl_remain;
	char     from[MSG_CALLSIGN_LEN + 1];
	char     to[MSG_CALLSIGN_LEN + 1];
	uint8_t  payload_len;
	uint8_t  payload[MSG_TEXT_LEN + 1];
} msg_packet_t;

// CRC-CCITT (poly 0x1021, init 0xFFFF, no reflection) - the GGM2 frame check.
uint16_t msg_packet_crc16(const uint8_t *data, uint16_t len);

// Build a packet into out (>= MSG_PKT_WIRE_LEN). Returns the byte count written, or 0 on a bad buffer. from NULL/empty
// falls back to "UVK1"; a TEXT message always goes to ALL (broadcast RF).
uint8_t msg_packet_build_text(uint8_t *out, uint8_t out_len, uint16_t id, const char *from, const char *text, uint8_t ttl_init);
uint8_t msg_packet_build_ack(uint8_t *out, uint8_t out_len, uint16_t id, const char *from, const char *to);
uint8_t msg_packet_build_ping(uint8_t *out, uint8_t out_len, uint16_t id, const char *from);
uint8_t msg_packet_build_wake(uint8_t *out, uint8_t out_len, uint16_t id, const char *from);
uint8_t msg_packet_build_pong(uint8_t *out, uint8_t out_len, uint16_t id, const char *from, const char *to, uint16_t battery_cv);

// Parse and verify (magic, version, CRC). False on any mismatch. pkt is filled on success.
bool msg_packet_parse(const uint8_t *data, uint8_t len, msg_packet_t *pkt);

// Self-test: builds and re-parses each type and checks the CRC. True when the wire format is intact.
bool msg_packet_selftest(void);

#endif
