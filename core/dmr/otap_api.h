// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
/*
 * Fleet administration service (CONFIG_OTAP, docs/spec/otap.md): the glue between the DMR data service and the radio
 * core. It holds the fleet's shared secret (set by the radio core from the settings, never on air) and checks every
 * command against it.
 *
 *   target   a signed datagram to us on OTAP_PORT is checked in the data service's receive path (otap_rx): ours, a
 *            secret set, the HMAC tag good, the sender an allowed admin, the counter above the last taken. Only then
 *            it waits as the pending request until the radio core's tick takes it (otap_take), runs it, and answers
 *            (otap_answer). The answer goes out signed from the tick. A failed check is counted and dropped
 *   console  the PC (over USB, debug link) builds and signs its own commands with the same secret; otap_console_send
 *            puts one on air from our ID, and replies heard for us wait in a small ring for otap_console_recv
 *
 * The secret, counter, flags and admin IDs are passed in with otap_set_config (as the data service takes its
 * settings), so this service links without the settings store and the host test runs it alone. When it accepts a
 * command it advances its counter; the radio core persists the new value (otap_counter) so a replay can't win after
 * a reboot.
 */
#ifndef RFOUNDRY_OTAP_API_H
#define RFOUNDRY_OTAP_API_H

#include <stdbool.h>
#include <stdint.h>
#include "dmr/otap.h"

#define OTAP_SEND_WAIT_MS	10000	// an answer or console command not sent by then is dropped
#define OTAP_SEND_TRIES		3	// a message is put on air this many times (unconfirmed), so a target that missed
					// the first burst still catches it; the duplicates are dropped by the replay counter
#define OTAP_SEND_GAP_MS	600	// between those tries, long enough for the other radio to answer in the gap
#define OTAP_CONSOLE_RING	4

typedef enum { OTAP_SEND_IDLE, OTAP_SEND_QUEUED, OTAP_SEND_SENT, OTAP_SEND_FAILED } otap_send_state_t;

// What a signed command may do on this radio (from settings_t.otap_flags)
#define OTAP_ALLOW_SETTINGS	0x01u	// talk / RX talkgroup, zone position
#define OTAP_ALLOW_KEYS		0x02u	// privacy key change (OTAR), inside the privacy bands only

typedef struct {
	uint8_t secret[OTAP_SECRET_LEN];	// all zeros: the service is off
	uint32_t counter;			// the highest request counter taken
	uint32_t admin[2];			// DMR IDs allowed to command us (0 = unused; both 0 = any secret holder)
	uint8_t flags;				// OTAP_ALLOW_*
} otap_config_t;

typedef struct {
	uint32_t rx, bad, off, bad_tag, replay, not_admin, busy, done, answers, answer_failed;
	uint32_t console_rx, console_dropped, console_sent, console_failed;
} otap_stats_t;

// A request taken off the air, checked, for the radio core to run (SET_KEY: payload decrypted)
typedef struct {
	uint8_t cmd;
	uint32_t counter;
	uint32_t src;
	uint8_t len;
	uint8_t payload[OTAP_PAYLOAD_MAX];
} otap_request_t;

// The radio task
void otap_init(void);
void otap_set_config(const otap_config_t *cfg);
// The counter as it stands (advanced when a command was accepted): the radio core saves it to settings when it grows
uint32_t otap_counter(void);
// The data service's receive hook (dmrDataOtapHook_t): a datagram to OTAP_PORT
void otap_rx(uint32_t src, bool group, uint32_t own_id, const uint8_t *payload, int length);
// Every data tick: our ID, whether we may send now. Sends a queued answer or console command when the channel lets us
void otap_tick(uint32_t own_id, bool can_send, uint32_t now_ms);
// The pending request for the radio core to run (false: none)
bool otap_take(otap_request_t *out);
// The answer to it: a result and what follows (status: OTAP_STATUS_LEN octets, else none)
void otap_answer(const otap_request_t *req, uint8_t result, const uint8_t *data, uint8_t len);

// Any task (the debug link): a message already built and signed by the PC, to dst
bool otap_console_send(uint32_t dst, const uint8_t *msg, int len);
otap_send_state_t otap_console_state(void);
// The next reply heard for us (0: none); the sender in *src
int otap_console_recv(uint32_t *src, uint8_t out[OTAP_MSG_MAX]);

void otap_get_stats(otap_stats_t *out);

#endif
