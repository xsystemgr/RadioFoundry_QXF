// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The fleet admin service (core/dmr/otapSvc.c): a signed command is taken, answered and its counter advanced; a bad
// tag, the wrong sender, a replay or a missing secret are dropped; the PC console path (docs/spec/otap.md)
#include <stdio.h>
#include <string.h>
#include "dmr/otap_api.h"
#include "dmr/otap.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

// --- the data service, stubbed: capture the last UDP datagram sent, and let the test fail the send ---
static struct { bool valid; bool group; uint32_t dst; uint16_t sp, dp; int len; uint8_t data[256]; } sent;
static bool send_ok = true;

void dmrDataServiceLock(void) {}
void dmrDataServiceUnlock(void) {}
bool dmrDataServiceSendUDP(bool group, uint32_t dst, uint16_t sp, uint16_t dp, const uint8_t *p, int len)
{
	if (!send_ok)
		return false;
	sent.valid = true; sent.group = group; sent.dst = dst; sent.sp = sp; sent.dp = dp; sent.len = len;
	memcpy(sent.data, p, (size_t)len);
	return true;
}

#define OUR_ID		0x100001
#define ADMIN_ID	0x2024AB
#define OTHER_ID	0x333333

static otapKeys_t keys;
static uint32_t ctr = 100;

static void provision(uint32_t a0, uint32_t a1, uint8_t flags)
{
	uint8_t secret[OTAP_SECRET_LEN];
	for (unsigned i = 0; i < OTAP_SECRET_LEN; i++)
		secret[i] = (uint8_t)(0x11u * (i + 1));
	otapDeriveKeys(secret, &keys);
	otap_config_t cfg = { .counter = ctr, .flags = flags };
	memcpy(cfg.secret, secret, sizeof cfg.secret);
	cfg.admin[0] = a0;
	cfg.admin[1] = a1;
	otap_set_config(&cfg);
}

// Deliver a signed request to us, from src, with counter
static void deliver(const otapKeys_t *k, uint8_t cmd, uint32_t counter, uint32_t src, const uint8_t *pl, uint8_t len)
{
	uint8_t msg[OTAP_MSG_MAX];
	int n = otapBuild(k, cmd, counter, OUR_ID, src, pl, len, msg);
	otap_rx(src, false, OUR_ID, msg, n);
}

static void run_tick(void)
{
	// Drive the whole send: the message goes out OTAP_SEND_TRIES times (unconfirmed retries), each after the gap,
	// and only then does the sent / answered counter advance
	uint32_t t = 1000;	// a channel we may send on
	for (int i = 0; i < OTAP_SEND_TRIES; i++) {
		otap_tick(OUR_ID, true, t);
		t += OTAP_SEND_GAP_MS;
	}
}

int main(void)
{
	otap_stats_t s;
	otap_request_t r;

	// A STATUS command from an allowed admin is taken, run and answered; the counter advances
	otap_init();
	provision(ADMIN_ID, 0, 0);
	sent.valid = false;
	deliver(&keys, OTAP_CMD_STATUS, 101, ADMIN_ID, NULL, 0);
	CHECK(otap_take(&r) && r.cmd == OTAP_CMD_STATUS && r.src == ADMIN_ID);
	CHECK(otap_counter() == 101);
	uint8_t status[OTAP_STATUS_LEN] = { OTAP_ST_DMR };
	otap_answer(&r, OTAP_R_OK, status, OTAP_STATUS_LEN);
	run_tick();
	CHECK(sent.valid && sent.dst == ADMIN_ID && sent.sp == OTAP_PORT && sent.dp == OTAP_PORT);
	// The answer is a signed reply to the same counter, result OK
	otapMsg_t m;
	CHECK(otapParse(sent.data, sent.len, &m) && (m.cmd & OTAP_REPLY) && m.counter == 101 && m.src == OUR_ID);
	CHECK(otapVerify(&keys, sent.data, sent.len) && m.payload[0] == OTAP_R_OK);
	otap_get_stats(&s);
	CHECK(s.rx == 1 && s.done == 1 && s.answers == 1);

	// A replay of the same counter is dropped
	deliver(&keys, OTAP_CMD_STATUS, 101, ADMIN_ID, NULL, 0);
	CHECK(!otap_take(&r));
	otap_get_stats(&s);
	CHECK(s.replay == 1);

	// A command from a radio not on the admin list is dropped
	deliver(&keys, OTAP_CMD_STATUS, 102, OTHER_ID, NULL, 0);
	CHECK(!otap_take(&r));
	otap_get_stats(&s);
	CHECK(s.not_admin == 1);

	// A forged tag (wrong secret) is dropped
	uint8_t bad[OTAP_SECRET_LEN] = { 1 };
	otapKeys_t kbad;
	otapDeriveKeys(bad, &kbad);
	deliver(&kbad, OTAP_CMD_STATUS, 103, ADMIN_ID, NULL, 0);
	CHECK(!otap_take(&r));
	otap_get_stats(&s);
	CHECK(s.bad_tag == 1);

	// With no admin list set, any holder of the secret is accepted
	otap_init();
	ctr = 0;
	provision(0, 0, OTAP_ALLOW_SETTINGS);
	deliver(&keys, OTAP_CMD_STATUS, 1, OTHER_ID, NULL, 0);
	CHECK(otap_take(&r) && r.src == OTHER_ID);

	// No secret: the service is off, every command dropped
	otap_init();
	otap_config_t off;
	memset(&off, 0, sizeof off);
	otap_set_config(&off);
	deliver(&keys, OTAP_CMD_STATUS, 50, ADMIN_ID, NULL, 0);
	CHECK(!otap_take(&r));
	otap_get_stats(&s);
	CHECK(s.off >= 1);

	// A reply heard for us goes to the console ring, not run as a command
	otap_init();
	ctr = 0;
	provision(ADMIN_ID, 0, 0);
	uint8_t body[1] = { OTAP_R_OK };
	deliver(&keys, OTAP_CMD_STATUS | OTAP_REPLY, 7, ADMIN_ID, body, 1);
	CHECK(!otap_take(&r));		// not a command
	uint32_t src = 0;
	uint8_t got[OTAP_MSG_MAX];
	int gn = otap_console_recv(&src, got);
	CHECK(gn > 0 && src == ADMIN_ID);
	CHECK(otap_console_recv(&src, got) == 0);	// drained

	// The console sends a command the PC built; it goes on air once; a second while busy is refused
	otap_init();
	uint8_t cmsg[OTAP_MSG_MAX];
	int cn = otapBuild(&keys, OTAP_CMD_STATUS, 200, 0x555555, OUR_ID, NULL, 0, cmsg);
	CHECK(otap_console_send(0x555555, cmsg, cn));
	CHECK(!otap_console_send(0x555555, cmsg, cn));	// one at a time
	sent.valid = false;
	run_tick();
	CHECK(sent.valid && sent.dst == 0x555555);
	otap_get_stats(&s);
	CHECK(s.console_sent == 1);

	// An answer that can't go out by the deadline fails instead of hanging
	otap_init();
	ctr = 0;
	provision(ADMIN_ID, 0, 0);
	deliver(&keys, OTAP_CMD_STATUS, 1, ADMIN_ID, NULL, 0);
	CHECK(otap_take(&r));
	otap_answer(&r, OTAP_R_OK, status, OTAP_STATUS_LEN);
	send_ok = false;
	otap_tick(OUR_ID, false, 1000);		// can't send: queued
	otap_tick(OUR_ID, true, 1000 + OTAP_SEND_WAIT_MS);	// past the deadline
	otap_get_stats(&s);
	CHECK(s.answer_failed == 1);
	send_ok = true;

	printf("# otapSvcTest: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
