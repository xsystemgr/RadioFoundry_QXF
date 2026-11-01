// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Fleet administration service on the data service: checking signed commands, answering, the PC console
// (otap_api.h, docs/spec/otap.md). Runs in the radio task, except otap_console_send / _recv (the host link task)
#include "config.h"
#if CONFIG_OTAP	// the whole file
#include <string.h>
#include "dmr/otap_api.h"
#include "dmr/dmrDataService.h"
#include "crypto/sha256.h"

#define UDP_HEADER	8

static otap_config_t cfg;		// the fleet secret and what it may do (from the radio core, settings-backed)
static otapKeys_t keys;			// derived from cfg.secret while a secret is set
static bool have_keys;
static uint32_t self_id;	// our DMR ID on this channel, for the answer's src
static otap_stats_t stats;

// The request taken off the air, waiting for the radio core's tick to run it
static bool pending;
static otap_request_t request;

// One outbound datagram at a time (an answer, or a console command): built, then sent when the channel lets us
static bool out_pending;
static bool out_console;		// for the stats: a console send, not an answer
static bool out_group;			// the pending send goes to a talkgroup (unconfirmed, no reply expected)
static uint32_t out_dst, out_at, out_last;
static uint8_t out_tries;
static uint8_t out_msg[UDP_HEADER + OTAP_MSG_MAX];	// the UDP header is filled by the data service; we hold payload
static uint8_t out_len;

// Replies heard for us, for the PC to read
static struct {
	uint32_t src;
	uint8_t len;
	uint8_t msg[OTAP_MSG_MAX];
} ring[OTAP_CONSOLE_RING];
static uint8_t ring_head, ring_count;

void otap_init(void)
{
	memset(&stats, 0, sizeof stats);
	pending = false;
	out_pending = false;
	ring_head = 0;
	ring_count = 0;
}

void otap_set_config(const otap_config_t *c)
{
	dmrDataServiceLock();
	cfg = *c;
	have_keys = otapSecretSet(cfg.secret);
	if (have_keys)
		otapDeriveKeys(cfg.secret, &keys);
	else
		crypto_wipe(&keys, sizeof keys);
	dmrDataServiceUnlock();
}

uint32_t otap_counter(void)
{
	return cfg.counter;
}

static bool admin_allowed(uint32_t src)
{
	if (cfg.admin[0] == 0 && cfg.admin[1] == 0)
		return true;		// no list: any sender who holds the secret
	return src == cfg.admin[0] || src == cfg.admin[1];
}

// A reply heard for us (the PC asked): keep it for otap_console_recv
static void ring_put(uint32_t src, const uint8_t *msg, int length)
{
	if (ring_count == OTAP_CONSOLE_RING) {
		stats.console_dropped++;
		return;
	}
	uint8_t i = (uint8_t)((ring_head + ring_count) % OTAP_CONSOLE_RING);
	ring[i].src = src;
	ring[i].len = (uint8_t)length;
	memcpy(ring[i].msg, msg, (size_t)length);
	ring_count++;
	stats.console_rx++;
}

void otap_rx(uint32_t src, bool group, uint32_t own_id, const uint8_t *payload, int length)
{
	otapMsg_t m;

	stats.rx++;
	self_id = own_id;
	if (!otapParse(payload, length, &m)) {
		stats.bad++;
		return;
	}
	if (!have_keys) {
		stats.off++;
		return;
	}
	if (!otapVerify(&keys, payload, length)) {
		stats.bad_tag++;
		return;
	}
	// A reply to a command the PC sent from us: it is for the console, not a command to run
	if (m.cmd & OTAP_REPLY) {
		dmrDataServiceLock();
		ring_put(src, payload, length);
		dmrDataServiceUnlock();
		return;
	}
	if (!admin_allowed(src)) {
		stats.not_admin++;
		return;
	}
	// The counter must be above every one taken: a captured command replayed later is refused
	if (m.counter <= cfg.counter) {
		stats.replay++;
		return;
	}
	if (pending) {
		stats.busy++;		// still running the last one; the sender retries
		return;
	}
	// Accept it: advance the counter now (the radio core persists it), so a replay can't win even if the apply fails
	cfg.counter = m.counter;
	request.cmd = m.cmd;
	request.group = group;	// a group command is run but not answered
	request.counter = m.counter;
	request.src = src;
	request.len = m.len;
	if (m.cmd == OTAP_CMD_SET_KEY)
		otapDecrypt(&keys, payload, &m, request.payload);
	else if (m.len > 0)
		memcpy(request.payload, m.payload, m.len);
	pending = true;
}

bool otap_take(otap_request_t *out)
{
	if (!pending)
		return false;
	*out = request;
	pending = false;
	stats.done++;
	if (request.cmd == OTAP_CMD_SET_KEY)
		crypto_wipe(request.payload, sizeof request.payload);
	return true;
}

void otap_answer(const otap_request_t *req, uint8_t result, const uint8_t *data, uint8_t len)
{
	uint8_t body[1 + OTAP_STATUS_LEN];
	uint8_t n;

	if (req->group)
		return;	// a group command is run on every radio at once; a reply from each would be a storm

	if (len > OTAP_STATUS_LEN)
		len = OTAP_STATUS_LEN;
	body[0] = result;
	if (len > 0)
		memcpy(&body[1], data, len);
	n = (uint8_t)(1u + len);

	dmrDataServiceLock();
	if (!out_pending && have_keys) {
		out_len = (uint8_t)otapBuild(&keys, (uint8_t)(req->cmd | OTAP_REPLY), req->counter, req->src, self_id, body,
					     n, &out_msg[UDP_HEADER]);
		out_dst = req->src;
		out_console = false;
		out_group = false;
		out_at = 0;
		out_pending = out_len > 0;
	}
	dmrDataServiceUnlock();
}

bool otap_console_send(uint32_t dst, bool group, const uint8_t *msg, int len)
{
	if (len <= 0 || len > OTAP_MSG_MAX || dst == 0 || dst > 0xFFFFFFu)
		return false;
	bool taken = false;
	dmrDataServiceLock();
	if (!out_pending) {
		memcpy(&out_msg[UDP_HEADER], msg, (size_t)len);
		out_len = (uint8_t)len;
		out_dst = dst;
		out_console = true;
		out_group = group;
		out_at = 0;
		out_pending = true;
		taken = true;
	}
	dmrDataServiceUnlock();
	return taken;
}

otap_send_state_t otap_console_state(void)
{
	return out_pending ? OTAP_SEND_QUEUED : OTAP_SEND_IDLE;
}

int otap_console_recv(uint32_t *src, uint8_t out[OTAP_MSG_MAX])
{
	int n = 0;
	dmrDataServiceLock();
	if (ring_count > 0) {
		if (src != NULL)
			*src = ring[ring_head].src;
		n = ring[ring_head].len;
		memcpy(out, ring[ring_head].msg, (size_t)n);
		ring_head = (uint8_t)((ring_head + 1) % OTAP_CONSOLE_RING);
		ring_count--;
	}
	dmrDataServiceUnlock();
	return n;
}

void otap_tick(uint32_t own_id, bool can_send, uint32_t now_ms)
{
	self_id = own_id;
	if (!out_pending)
		return;
	if (out_at == 0) {
		out_at = now_ms ? now_ms : 1;
		out_last = 0;
		out_tries = 0;
	}
	// Give up if the channel never let any copy out in the window
	if ((now_ms - out_at) >= OTAP_SEND_WAIT_MS) {
		dmrDataServiceLock();
		out_pending = false;
		dmrDataServiceUnlock();
		if (out_tries > 0) {
			if (out_console) stats.console_sent++; else stats.answers++;
		} else {
			if (out_console) stats.console_failed++; else stats.answer_failed++;
		}
		return;
	}
	if (!can_send)
		return;
	if ((out_last != 0) && ((now_ms - out_last) < OTAP_SEND_GAP_MS))
		return;	// wait between tries, so the other radio can answer in the gap
	if (dmrDataServiceSendUDP(out_group, out_dst, OTAP_PORT, OTAP_PORT, &out_msg[UDP_HEADER], out_len)) {
		out_last = now_ms ? now_ms : 1;
		out_tries++;
		if (out_tries >= OTAP_SEND_TRIES) {
			dmrDataServiceLock();
			out_pending = false;
			dmrDataServiceUnlock();
			if (out_console) stats.console_sent++; else stats.answers++;
		}
	}
}

void otap_get_stats(otap_stats_t *out)
{
	*out = stats;
}

#endif	// CONFIG_OTAP
