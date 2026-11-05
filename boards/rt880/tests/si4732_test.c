// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Host test for the Si4732 HF driver (drivers/bcast/si4732/si4732.c) with a mock chip transport. Checks the POWER_UP
// front-end selection, the AM vs FM tune encoding and units, the band-change re-power, and the tune-status parsing.
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "bcast/si4732/si4732.h"

#define MAX_CMDS 32
static uint8_t cmd_log[MAX_CMDS][8];
static size_t cmd_len[MAX_CMDS];
static size_t cmd_n;
static uint8_t last_op;	// opcode of the most recent command, so resp() can return the right reply layout

static bool mock_cmd(void *ctx, const uint8_t *buf, size_t n)
{
	(void)ctx;
	assert(cmd_n < MAX_CMDS);
	assert(n <= 8);
	memcpy(cmd_log[cmd_n], buf, n);
	cmd_len[cmd_n] = n;
	cmd_n++;
	last_op = buf[0];
	return true;
}
// Each command has its own reply layout. byte 0 is the status (bit7 CTS, set so CTS polls pass). RSQ_STATUS carries
// the live RSSI at byte 3; TUNE_STATUS carries the frequency at bytes 2-3. Everything else just needs CTS.
static bool mock_resp(void *ctx, uint8_t *buf, size_t n)
{
	(void)ctx;
	uint8_t reply[8] = { 0x80, 0, 0, 0, 0, 0, 0, 0 };
	if (last_op == 0x23 || last_op == 0x43) {		// FM/AM RSQ_STATUS
		reply[1] = 0x01;				// VALID
		reply[3] = 55;					// RSSI (dBuV)
		reply[4] = 20;					// SNR
	} else if (last_op == 0x22 || last_op == 0x42) {	// FM/AM TUNE_STATUS
		reply[1] = 0x01;
		reply[2] = 0x1B;				// freq hi
		reply[3] = 0xBC;				// freq lo -> 0x1BBC = 7100
	} else {						// CTS / GET_REV etc.
		reply[0] = 0x81;
		reply[1] = 32;					// GET_REV part number
	}
	for (size_t i = 0; i < n; i++) {
		buf[i] = reply[i];
	}
	return true;
}
static void mock_delay(uint32_t ms) { (void)ms; }

static const si4732_bus_t mock_bus = { mock_cmd, mock_resp, mock_delay, NULL };

// The first logged command whose opcode is byte 0 == op, or NULL
static const uint8_t *find_cmd(uint8_t op)
{
	for (size_t i = 0; i < cmd_n; i++) {
		if (cmd_len[i] >= 1 && cmd_log[i][0] == op) {
			return cmd_log[i];
		}
	}
	return NULL;
}
static void reset_log(void) { cmd_n = 0; }

#define CMD_POWER_UP		0x01u
#define CMD_POWER_DOWN		0x11u
#define CMD_FM_TUNE_FREQ	0x20u
#define CMD_AM_TUNE_FREQ	0x40u
#define CMD_AM_TUNE_STATUS	0x42u
#define CMD_AM_RSQ_STATUS	0x43u
#define CMD_GET_INT_STATUS	0x14u

int main(void)
{
	si4732_t d;
	si4732_attach(&d, &mock_bus);
	assert(!d.powered);

	// A tune at or below 30 MHz uses the AM front end, frequency in 1 kHz units, and powers the chip up first.
	reset_log();
	assert(si4732_tune(&d, 7100u));
	assert(d.powered && d.band == SI4732_BAND_AM);
	const uint8_t *pu = find_cmd(CMD_POWER_UP);
	assert(pu != NULL);
	assert(pu[1] == (0x10u | 0x01u));	// XOSCEN | FUNC_AM
	assert(pu[2] == 0x05u);			// analog audio out
	const uint8_t *amt = find_cmd(CMD_AM_TUNE_FREQ);
	assert(amt != NULL);
	assert(((uint16_t)amt[2] << 8 | amt[3]) == 7100u);	// 1 kHz units: the frequency itself

	// A tune above 30 MHz switches to the FM front end: power down, power up FUNC_FM, FM freq in 10 kHz units.
	reset_log();
	assert(si4732_tune(&d, 100000u));	// 100 MHz
	assert(d.powered && d.band == SI4732_BAND_FM);
	assert(find_cmd(CMD_POWER_DOWN) != NULL);
	pu = find_cmd(CMD_POWER_UP);
	assert(pu != NULL && pu[1] == (0x10u | 0x00u));		// XOSCEN | FUNC_FM
	const uint8_t *fmt = find_cmd(CMD_FM_TUNE_FREQ);
	assert(fmt != NULL);
	assert(((uint16_t)fmt[2] << 8 | fmt[3]) == 10000u);	// 100000 kHz / 10 = 10000

	// Staying within the AM range does not re-power (no new POWER_UP logged).
	reset_log();
	assert(si4732_tune(&d, 3500u));		// 80 m, still AM
	assert(d.band == SI4732_BAND_AM);
	reset_log();
	assert(si4732_tune(&d, 3600u));
	assert(find_cmd(CMD_POWER_UP) == NULL);	// same band: just an AM_TUNE_FREQ
	assert(find_cmd(CMD_AM_TUNE_FREQ) != NULL);

	// status() reads live RSSI from RSQ_STATUS and the frequency from TUNE_STATUS: rssi 55, 7100 kHz, valid.
	uint32_t khz; uint8_t rssi; bool valid, stereo, tuning;
	reset_log();
	assert(si4732_status(&d, &khz, &rssi, &valid, &stereo, &tuning));
	assert(find_cmd(CMD_AM_RSQ_STATUS) != NULL);	// live signal read, not gated on tune-complete
	assert(find_cmd(CMD_AM_TUNE_STATUS) != NULL);
	assert(rssi == 55u);			// from RSQ RESP3, the live value
	assert(khz == 7100u);			// AM: 1 kHz units, from TUNE_STATUS
	assert(valid);
	assert(!stereo);			// AM is never stereo
	assert(!tuning);

	// RSSI is read every call (it no longer latches once and sticks): a second read still returns the live value.
	rssi = 0;
	assert(si4732_status(&d, NULL, &rssi, NULL, NULL, NULL));
	assert(rssi == 55u);

	// Power down clears the powered flag.
	assert(si4732_power_down(&d));
	assert(!d.powered);

	printf("si4732 test: OK (%s)\n", "power-up, AM/FM units, band switch, status");
	return 0;
}
