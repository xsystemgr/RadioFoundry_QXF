// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The fleet admin message layer (core/dmr/otap.c): key derivation, signing, verifying, the privacy-key encryption,
// and that a tampered or wrong-secret message is refused (docs/spec/otap.md)
#include <stdio.h>
#include <string.h>
#include "dmr/otap.h"
#include "crypto/sha256.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

int main(void)
{
	uint8_t secret[OTAP_SECRET_LEN];
	for (unsigned i = 0; i < OTAP_SECRET_LEN; i++)
		secret[i] = (uint8_t)(0x11u * (i + 1));
	otapKeys_t k;
	otapDeriveKeys(secret, &k);

	// A secret of all zeros is "off"; any nonzero octet is "set"
	uint8_t zero[OTAP_SECRET_LEN] = { 0 };
	CHECK(!otapSecretSet(zero));
	CHECK(otapSecretSet(secret));

	// Ka and Ke differ (different labels)
	CHECK(memcmp(k.ka, k.ke, 32) != 0);

	// Build a STATUS request and read it back
	uint8_t msg[OTAP_MSG_MAX];
	int n = otapBuild(&k, OTAP_CMD_STATUS, 5, 0x2024AB, 0x100001, NULL, 0, msg);
	CHECK(n == OTAP_HEADER + OTAP_TAG);
	otapMsg_t m;
	CHECK(otapParse(msg, n, &m));
	CHECK(m.cmd == OTAP_CMD_STATUS && m.counter == 5 && m.dst == 0x2024AB && m.src == 0x100001 && m.len == 0);
	CHECK(otapVerify(&k, msg, n));

	// A command with a payload
	uint8_t tg[3] = { 0x00, 0x08, 0x91 };	// talkgroup 2193
	n = otapBuild(&k, OTAP_CMD_TALK_TG, 6, 0x2024AB, 0x100001, tg, 3, msg);
	CHECK(n == OTAP_HEADER + 3 + OTAP_TAG);
	CHECK(otapParse(msg, n, &m) && m.len == 3 && memcmp(m.payload, tg, 3) == 0);
	CHECK(otapVerify(&k, msg, n));

	// Any single-bit change anywhere makes the tag fail
	for (int i = 0; i < n; i++) {
		uint8_t save = msg[i];
		msg[i] ^= 0x01;
		CHECK(!otapVerify(&k, msg, n));
		msg[i] = save;
	}
	CHECK(otapVerify(&k, msg, n));	// restored

	// A radio with a different secret refuses it
	uint8_t other[OTAP_SECRET_LEN];
	memcpy(other, secret, sizeof other);
	other[0] ^= 0x80;
	otapKeys_t k2;
	otapDeriveKeys(other, &k2);
	CHECK(!otapVerify(&k2, msg, n));

	// SET_KEY: the key is encrypted on the wire and decrypts back; it is not in the clear
	uint8_t key_payload[4] = { 1 /* Motorola */, 0x00, 0x2A /* key 42 */, 1 /* activate */ };
	n = otapBuild(&k, OTAP_CMD_SET_KEY, 7, 0x2024AB, 0x100001, key_payload, 4, msg);
	CHECK(otapParse(msg, n, &m) && m.len == 4);
	CHECK(otapVerify(&k, msg, n));
	CHECK(memcmp(m.payload, key_payload, 4) != 0);	// not plaintext on the wire
	uint8_t dec[OTAP_PAYLOAD_MAX];
	otapDecrypt(&k, msg, &m, dec);
	CHECK(memcmp(dec, key_payload, 4) == 0);		// recovered with the secret
	// The wrong secret recovers something else (and the tag already failed)
	uint8_t dec2[OTAP_PAYLOAD_MAX];
	otapDecrypt(&k2, msg, &m, dec2);
	CHECK(memcmp(dec2, key_payload, 4) != 0);

	// A reply carries the request's counter and the reply bit
	uint8_t body[1 + OTAP_STATUS_LEN] = { OTAP_R_OK };
	n = otapBuild(&k, OTAP_CMD_STATUS | OTAP_REPLY, 5, 0x100001, 0x2024AB, body, 1 + OTAP_STATUS_LEN, msg);
	CHECK(otapParse(msg, n, &m) && (m.cmd & OTAP_REPLY) && m.counter == 5);
	CHECK(otapVerify(&k, msg, n));

	// Junk and truncation are refused
	CHECK(!otapParse((const uint8_t *)"RA", 2, &m));
	CHECK(!otapParse(msg, OTAP_HEADER + OTAP_TAG - 1, &m));
	uint8_t bad[OTAP_MSG_MAX];
	memcpy(bad, msg, n);
	bad[14] = OTAP_PAYLOAD_MAX + 1;		// a length past the max
	CHECK(!otapParse(bad, n, &m));
	bad[0] = 'X';				// wrong magic
	CHECK(!otapParse(bad, n, &m));

	// A payload at the limit builds; one over it is refused
	uint8_t big[OTAP_PAYLOAD_MAX + 1] = { 0 };
	CHECK(otapBuild(&k, OTAP_CMD_STATUS, 1, 1, 2, big, OTAP_PAYLOAD_MAX, msg) == OTAP_MSG_MAX);
	CHECK(otapBuild(&k, OTAP_CMD_STATUS, 1, 1, 2, big, OTAP_PAYLOAD_MAX + 1, msg) == 0);

	printf("# otapTest: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
