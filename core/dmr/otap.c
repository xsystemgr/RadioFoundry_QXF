// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Fleet admin messages (otap.h, docs/spec/otap.md)
#include <string.h>
#include "dmr/otap.h"
#include "crypto/sha256.h"

static const char LABEL_AUTH[] = "RF-OTAP auth";
static const char LABEL_ENC[] = "RF-OTAP enc";

void otapDeriveKeys(const uint8_t secret[OTAP_SECRET_LEN], otapKeys_t *k)
{
	hmac_sha256(secret, OTAP_SECRET_LEN, LABEL_AUTH, sizeof LABEL_AUTH - 1u, k->ka);
	hmac_sha256(secret, OTAP_SECRET_LEN, LABEL_ENC, sizeof LABEL_ENC - 1u, k->ke);
}

bool otapSecretSet(const uint8_t secret[OTAP_SECRET_LEN])
{
	uint8_t any = 0;
	for (unsigned i = 0; i < OTAP_SECRET_LEN; i++)
		any |= secret[i];
	return any != 0;
}

static void put24(uint8_t *p, uint32_t v) { p[0]=(uint8_t)(v>>16); p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)v; }
static uint32_t get24(const uint8_t *p) { return ((uint32_t)p[0]<<16)|((uint32_t)p[1]<<8)|p[2]; }

static void tag(const otapKeys_t *k, const uint8_t *msg, int n, uint8_t out[SHA256_LEN])
{
	hmac_sha256(k->ka, sizeof k->ka, msg, (size_t)n, out);
}

static void keystream(const otapKeys_t *k, const uint8_t *header, uint8_t out[SHA256_LEN])
{
	hmac_sha256(k->ke, sizeof k->ke, header, OTAP_HEADER, out);
}

int otapBuild(const otapKeys_t *k, uint8_t cmd, uint32_t counter, uint32_t dst, uint32_t src, const uint8_t *payload,
	      uint8_t len, uint8_t *out)
{
	uint8_t t[SHA256_LEN];

	if (len > OTAP_PAYLOAD_MAX)
		return 0;
	out[0] = 'R'; out[1] = 'A'; out[2] = OTAP_VERSION; out[3] = cmd;
	out[4] = (uint8_t)(counter >> 24); out[5] = (uint8_t)(counter >> 16);
	out[6] = (uint8_t)(counter >> 8);  out[7] = (uint8_t)counter;
	put24(&out[8], dst);
	put24(&out[11], src);
	out[14] = len;
	if (len > 0)
		memcpy(&out[OTAP_HEADER], payload, len);
	if (cmd == OTAP_CMD_SET_KEY) {	// only the request carries a key; the reply is a plain result byte
		keystream(k, out, t);
		for (unsigned i = 0; i < len; i++)
			out[OTAP_HEADER + i] ^= t[i];
	}
	tag(k, out, OTAP_HEADER + len, t);
	memcpy(&out[OTAP_HEADER + len], t, OTAP_TAG);
	crypto_wipe(t, sizeof t);
	return OTAP_HEADER + len + OTAP_TAG;
}

bool otapParse(const uint8_t *msg, int length, otapMsg_t *m)
{
	if (length < OTAP_HEADER + OTAP_TAG || msg[0] != 'R' || msg[1] != 'A' || msg[2] != OTAP_VERSION)
		return false;
	if (msg[14] > OTAP_PAYLOAD_MAX || length != OTAP_HEADER + msg[14] + OTAP_TAG)
		return false;
	m->cmd = msg[3];
	m->counter = ((uint32_t)msg[4]<<24)|((uint32_t)msg[5]<<16)|((uint32_t)msg[6]<<8)|msg[7];
	m->dst = get24(&msg[8]);
	m->src = get24(&msg[11]);
	m->len = msg[14];
	m->payload = &msg[OTAP_HEADER];
	return true;
}

bool otapVerify(const otapKeys_t *k, const uint8_t *msg, int length)
{
	uint8_t t[SHA256_LEN];
	if (length < OTAP_HEADER + OTAP_TAG)
		return false;
	tag(k, msg, length - OTAP_TAG, t);
	bool ok = crypto_equal(t, &msg[length - OTAP_TAG], OTAP_TAG) != 0;
	crypto_wipe(t, sizeof t);
	return ok;
}

void otapDecrypt(const otapKeys_t *k, const uint8_t *msg, const otapMsg_t *m, uint8_t *out)
{
	uint8_t t[SHA256_LEN];
	keystream(k, msg, t);
	for (unsigned i = 0; i < m->len; i++)
		out[i] = (uint8_t)(m->payload[i] ^ t[i]);
	crypto_wipe(t, sizeof t);
}
