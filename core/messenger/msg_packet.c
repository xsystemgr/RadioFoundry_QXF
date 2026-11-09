// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// GOGUFW-compatible GGM2 messenger packet (msg_packet.h). Ported wire-for-wire from the GOGUFW firmware.
#include <string.h>
#include "messenger/msg_packet.h"

static void put_u16_le(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v & 0xFFu);
	p[1] = (uint8_t)(v >> 8);
}

static uint16_t get_u16_le(const uint8_t *p)
{
	return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

uint16_t msg_packet_crc16(const uint8_t *data, uint16_t len)
{
	uint16_t crc = 0xFFFFu;
	while (len--) {
		crc ^= (uint16_t)(*data++) << 8;
		for (uint8_t i = 0; i < 8; i++) {
			crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
		}
	}
	return crc;
}

// Copy a C string into a fixed-width, zero-padded on-wire field (no terminator kept).
static void copy_field(uint8_t *dst, uint8_t dst_len, const char *src)
{
	memset(dst, 0, dst_len);
	if (src == NULL) {
		return;
	}
	for (uint8_t i = 0; i < dst_len && src[i]; i++) {
		dst[i] = (uint8_t)src[i];
	}
}

// Common header for every packet. Leaves payload_len (out[27]) and payload at 0; the caller fills and CRCs.
static void build_header(uint8_t *out, uint8_t type, uint16_t id, const char *from, const char *to,
	uint8_t ttl_init, uint8_t ttl_remain)
{
	memset(out, 0, MSG_PKT_WIRE_LEN);
	out[0] = MSG_PKT_MAGIC0;
	out[1] = MSG_PKT_MAGIC1;
	out[2] = MSG_PKT_MAGIC2;
	out[3] = MSG_PKT_MAGIC3;
	out[4] = MSG_PKT_VERSION;
	out[5] = type;
	out[6] = 0;		// flags, reserved
	put_u16_le(&out[7], id);
	out[9] = ttl_init;
	out[10] = ttl_remain;
	copy_field(&out[11], MSG_CALLSIGN_LEN, (from && from[0]) ? from : MSG_PKT_DEFAULT_FROM);
	copy_field(&out[19], MSG_CALLSIGN_LEN, (to && to[0]) ? to : MSG_PKT_TO_ALL);
}

static void finalize(uint8_t *out)
{
	uint16_t crc = msg_packet_crc16(out, MSG_PKT_WIRE_LEN - 2u);
	put_u16_le(&out[MSG_PKT_WIRE_LEN - 2u], crc);
}

uint8_t msg_packet_build_text(uint8_t *out, uint8_t out_len, uint16_t id, const char *from, const char *text,
	uint8_t ttl_init)
{
	if (out == NULL || out_len < MSG_PKT_WIRE_LEN) {
		return 0;
	}
	build_header(out, MSG_PKT_TYPE_TEXT, id, from, MSG_PKT_TO_ALL, ttl_init, ttl_init);
	uint8_t text_len = 0;
	if (text != NULL) {
		while (text[text_len] && text_len < MSG_PKT_MAX_PAYLOAD) {
			text_len++;
		}
		memcpy(&out[28], text, text_len);
	}
	out[27] = text_len;
	finalize(out);
	return MSG_PKT_WIRE_LEN;
}

uint8_t msg_packet_build_ack(uint8_t *out, uint8_t out_len, uint16_t id, const char *from, const char *to)
{
	if (out == NULL || out_len < MSG_PKT_WIRE_LEN) {
		return 0;
	}
	build_header(out, MSG_PKT_TYPE_ACK, id, from, to, 1, 1);
	// Mirror the ACKed message id in the payload too, for a robust fallback match (GOGUFW RF21).
	out[27] = 2;
	out[28] = (uint8_t)(id & 0xFFu);
	out[29] = (uint8_t)(id >> 8);
	finalize(out);
	return MSG_PKT_WIRE_LEN;
}

static uint8_t build_control(uint8_t *out, uint8_t out_len, uint8_t type, uint16_t id, const char *from, const char *to)
{
	if (out == NULL || out_len < MSG_PKT_WIRE_LEN) {
		return 0;
	}
	build_header(out, type, id, from, to, 1, 1);
	out[27] = 0;
	finalize(out);
	return MSG_PKT_WIRE_LEN;
}

uint8_t msg_packet_build_ping(uint8_t *out, uint8_t out_len, uint16_t id, const char *from)
{
	return build_control(out, out_len, MSG_PKT_TYPE_PING, id, from, MSG_PKT_TO_ALL);
}

uint8_t msg_packet_build_wake(uint8_t *out, uint8_t out_len, uint16_t id, const char *from)
{
	return build_control(out, out_len, MSG_PKT_TYPE_WAKE, id, from, MSG_PKT_TO_ALL);
}

uint8_t msg_packet_build_pong(uint8_t *out, uint8_t out_len, uint16_t id, const char *from, const char *to,
	uint16_t battery_cv)
{
	uint8_t n = build_control(out, out_len, MSG_PKT_TYPE_PONG, id, from, (to && to[0]) ? to : MSG_PKT_TO_ALL);
	if (n == MSG_PKT_WIRE_LEN) {
		out[27] = 2;		// carry remote battery centivolts (GOGUFW Range Check)
		out[28] = (uint8_t)(battery_cv & 0xFFu);
		out[29] = (uint8_t)(battery_cv >> 8);
		finalize(out);
	}
	return n;
}

static void read_field(char *dst, const uint8_t *src)
{
	memcpy(dst, src, MSG_CALLSIGN_LEN);
	dst[MSG_CALLSIGN_LEN] = 0;
}

bool msg_packet_parse(const uint8_t *data, uint8_t len, msg_packet_t *pkt)
{
	if (data == NULL || pkt == NULL || len < MSG_PKT_WIRE_LEN) {
		return false;
	}
	if (data[0] != MSG_PKT_MAGIC0 || data[1] != MSG_PKT_MAGIC1 ||
	    data[2] != MSG_PKT_MAGIC2 || data[3] != MSG_PKT_MAGIC3 || data[4] != MSG_PKT_VERSION) {
		return false;
	}
	uint16_t crc = msg_packet_crc16(data, MSG_PKT_WIRE_LEN - 2u);
	if (crc != get_u16_le(&data[MSG_PKT_WIRE_LEN - 2u])) {
		return false;
	}
	pkt->type = data[5];
	pkt->flags = data[6];
	pkt->id = get_u16_le(&data[7]);
	pkt->ttl_init = data[9];
	pkt->ttl_remain = data[10];
	read_field(pkt->from, &data[11]);
	read_field(pkt->to, &data[19]);
	uint8_t pl = data[27];
	if (pl > MSG_TEXT_LEN) {
		pl = MSG_TEXT_LEN;
	}
	pkt->payload_len = pl;
	memcpy(pkt->payload, &data[28], pl);
	pkt->payload[pl] = 0;
	return true;
}

bool msg_packet_selftest(void)
{
	uint8_t buf[MSG_PKT_WIRE_LEN];
	msg_packet_t p;

	if (msg_packet_build_text(buf, sizeof buf, 0x1234u, "SV4QXF", "Hello", 3) != MSG_PKT_WIRE_LEN) {
		return false;
	}
	if (buf[0] != 'G' || buf[1] != 'G' || buf[2] != 'M' || buf[3] != '2' || buf[4] != 2u || buf[5] != 1u) {
		return false;
	}
	if (!msg_packet_parse(buf, sizeof buf, &p) || p.type != MSG_PKT_TYPE_TEXT || p.id != 0x1234u) {
		return false;
	}
	if (p.payload_len != 5 || memcmp(p.payload, "Hello", 5) != 0 || strcmp(p.from, "SV4QXF") != 0) {
		return false;
	}
	if (strcmp(p.to, "ALL") != 0 || p.ttl_init != 3) {
		return false;
	}
	// A single flipped byte must fail the CRC.
	buf[28] ^= 0x01u;
	if (msg_packet_parse(buf, sizeof buf, &p)) {
		return false;
	}
	if (msg_packet_build_ack(buf, sizeof buf, 0x1234u, "SV4QXF", "UVK1") != MSG_PKT_WIRE_LEN ||
	    !msg_packet_parse(buf, sizeof buf, &p) || p.type != MSG_PKT_TYPE_ACK) {
		return false;
	}
	return true;
}
