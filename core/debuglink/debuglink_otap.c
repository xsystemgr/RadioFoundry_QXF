// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Fleet administration on the debug link (docs/spec/debuglink.md 3.23, docs/spec/otap.md 6): provisioning this radio
// (the shared secret over USB, the policy flags, the admin IDs) and the PC console that puts signed commands on air
// and reads the replies. The PC holds the secret and builds / checks the messages; the radio relays them. Runs in the
// host link task; otap_console_send / _recv lock the data service.
#include <string.h>
#include "config.h"
#include "debuglink/debuglink.h"
#include "debuglink/debuglink_proto.h"
#if CONFIG_OTAP
#include "dmr/otap_api.h"
#include "settings/settings.h"
#include "radio/radio.h"
#include "crypto/sha256.h"	// crypto_wipe

static uint32_t get24(const uint8_t *p) { return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2]; }
static void put24(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 16); p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)v; }

size_t debuglink_cmd_otap(const uint8_t *req, uint16_t len, uint8_t *out, size_t cap)
{
	if (len < 1) {
		out[0] = DL_ST_BAD_ARG;
		return 1;
	}
	const uint8_t *a = req + 1;
	uint16_t n = (uint16_t)(len - 1u);

	switch (req[0]) {
	case DL_OTAP_OP_INFO: {
		settings_t st;
		settings_read(&st);
		uint8_t set = otapSecretSet(st.otap_secret) ? 1u : 0u;
		out[0] = DL_ST_OK;
		out[1] = st.otap_flags;
		out[2] = (uint8_t)(st.otap_counter >> 24); out[3] = (uint8_t)(st.otap_counter >> 16);
		out[4] = (uint8_t)(st.otap_counter >> 8); out[5] = (uint8_t)st.otap_counter;
		put24(&out[6], st.otap_admin[0] & 0xFFFFFFu);
		put24(&out[9], st.otap_admin[1] & 0xFFFFFFu);
		out[12] = set;
		crypto_wipe(&st, sizeof st);
		return 13;
	}
	case DL_OTAP_OP_SECRET:
		// 32 bytes set the secret; 0 bytes clear it (the service off). Never read back
		if (n != OTAP_SECRET_LEN && n != 0) {
			out[0] = DL_ST_BAD_ARG;
			return 1;
		}
		{
			settings_t *st = settings_begin();
			if (n == OTAP_SECRET_LEN)
				memcpy(st->otap_secret, a, OTAP_SECRET_LEN);
			else
				memset(st->otap_secret, 0, OTAP_SECRET_LEN);
			st->otap_counter = 0;	// a new secret starts the replay counter over
			settings_end(0);
		}
		radio_req_reload();	// the radio core re-reads the secret into the service
		out[0] = DL_ST_OK;
		return 1;
	case DL_OTAP_OP_FLAGS:
		if (n != 1) { out[0] = DL_ST_BAD_ARG; return 1; }
		{
			settings_t *st = settings_begin();
			st->otap_flags = a[0] & (SETTINGS_OTAP_SETTINGS | SETTINGS_OTAP_KEYS);
			settings_end(0);
		}
		radio_req_reload();
		out[0] = DL_ST_OK;
		return 1;
	case DL_OTAP_OP_ADMIN:
		if (n != 4 || a[0] > 1) { out[0] = DL_ST_BAD_ARG; return 1; }
		{
			settings_t *st = settings_begin();
			st->otap_admin[a[0]] = get24(a + 1);
			settings_end(0);
		}
		radio_req_reload();
		out[0] = DL_ST_OK;
		return 1;
	case DL_OTAP_OP_SEND: {
		// A whole signed message built by the PC; its destination is the target ID in the header (otap.h)
		otapMsg_t m;
		if (!otapParse(a, n, &m)) { out[0] = DL_ST_BAD_ARG; return 1; }
		out[0] = otap_console_send(m.dst, a, n) ? DL_ST_OK : DL_ST_BUSY;
		return 1;
	}
	case DL_OTAP_OP_RECV: {
		uint32_t src = 0;
		uint8_t msg[OTAP_MSG_MAX];
		int got = otap_console_recv(&src, msg);
		out[0] = DL_ST_OK;
		if (got <= 0) {
			out[1] = 0;	// no reply waiting
			return 2;
		}
		out[1] = 1;
		put24(&out[2], src);
		out[5] = (uint8_t)got;
		if ((size_t)(6 + got) > cap) { out[0] = DL_ST_TOO_LONG; return 1; }
		memcpy(&out[6], msg, (size_t)got);
		return (size_t)(6 + got);
	}
	default:
		out[0] = DL_ST_BAD_ARG;
		return 1;
	}
}

#else

size_t debuglink_cmd_otap(const uint8_t *req, uint16_t len, uint8_t *out, size_t cap)
{
	(void)req; (void)len; (void)cap;
	out[0] = DL_ST_UNAVAILABLE;
	return 1;
}

#endif
