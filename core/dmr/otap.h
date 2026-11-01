// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
/*
 * Fleet administration over DMR data (CONFIG_OTAP, docs/spec/otap.md): one owner's radios share a 32-octet secret,
 * loaded into each over USB and never sent on air. A command is obeyed only when its HMAC over that secret checks
 * out, so a radio answers its own fleet and no one else; this is the fleet-management model of commercial OTAP/OTAR
 * (not a way to reach radios one hasn't provisioned, which hold a different secret or none).
 *
 * This file is the message: its layout, the HMAC tag, the privacy-key encryption and the checks. Pure encode / decode,
 * so the host tests run it as is. Everything decoded here came off the air from anyone: every length is checked and a
 * message whose tag fails is dropped.
 *
 * A message is a UDP datagram on OTAP_PORT, to one radio:
 *
 *   0  2  magic "RA"          8  3  the target's DMR ID         15  n   payload (n <= OTAP_PAYLOAD_MAX)
 *   2  1  version (1)        11  3  the sender's DMR ID         15+n 16 tag: HMAC-SHA256(Ka, octets 0..15+n)[0..16)
 *   3  1  command (bit 7: a reply)
 *   4  4  counter (big endian): a request's is above every one the target took; a reply carries its request's
 *   14 1  n
 *
 * Ka = HMAC(secret, "RF-OTAP auth") and Ke = HMAC(secret, "RF-OTAP enc") come from the shared secret. SET_KEY's
 * payload is XORed with HMAC(Ke, octets 0..15), so a captured message never shows the new privacy key.
 */
#ifndef RFOUNDRY_OTAP_H
#define RFOUNDRY_OTAP_H

#include <stdbool.h>
#include <stdint.h>

#define OTAP_PORT		4101	// UDP source and destination port (beside zone sharing's 4100)
#define OTAP_VERSION		1
#define OTAP_HEADER		15
#define OTAP_TAG		16
#define OTAP_PAYLOAD_MAX	28
#define OTAP_MSG_MAX		(OTAP_HEADER + OTAP_PAYLOAD_MAX + OTAP_TAG)
#define OTAP_SECRET_LEN		32
#define OTAP_REPLY		0x80u

// Commands. A reply is (command | OTAP_REPLY); its payload starts with an OTAP_R_* result
#define OTAP_CMD_STATUS		0x01	// -> status (OTAP_STATUS_LEN octets after the result)
#define OTAP_CMD_TALK_TG	0x02	// u24 talkgroup: the talk target for now (0: the channel's own)
#define OTAP_CMD_RX_TG		0x03	// u24 talkgroup heard besides the talk target (0: none)
#define OTAP_CMD_ZONE_POS	0x04	// u16 position in the zone (0-based): switch that channel
#define OTAP_CMD_SET_KEY	0x10	// encrypted payload: type u8 (dmrPrivacyType_t), key u16, activate u8 (OTAR)
#define OTAP_CMD_PRIVACY	0x11	// u8 1: the stored key on, 0: privacy off

// Results
#define OTAP_R_OK		0
#define OTAP_R_REFUSED		1	// this command is switched off on the target
#define OTAP_R_BAD_ARG		2
#define OTAP_R_BAND		3	// a key change outside the target's privacy bands
#define OTAP_R_FAILED		4
#define OTAP_R_UNKNOWN		5	// a command the target doesn't know

// Status reply payload after the result (big endian): flags u8, rx Hz u32, tx Hz u32, talk target u24, RX TG u24,
// battery mV u16, channel number u16, privacy type u8
#define OTAP_STATUS_LEN		21
#define OTAP_ST_DMR		0x01
#define OTAP_ST_VFO		0x02
#define OTAP_ST_PRIVACY		0x04	// a key is loaded
#define OTAP_ST_PRIVACY_TX	0x08	// ... and calls go out encrypted here (the frequency is in a privacy band)
#define OTAP_ST_OTAR		0x20	// key changes are taken
#define OTAP_ST_SETTINGS	0x40	// setting changes are taken

typedef struct {
	uint8_t cmd;
	uint32_t counter;
	uint32_t dst, src;
	uint8_t len;
	const uint8_t *payload;		// into the message
} otapMsg_t;

typedef struct {
	uint8_t ka[32];
	uint8_t ke[32];
} otapKeys_t;

void otapDeriveKeys(const uint8_t secret[OTAP_SECRET_LEN], otapKeys_t *k);
// Whether a secret is set (all zeros: none, the service is off)
bool otapSecretSet(const uint8_t secret[OTAP_SECRET_LEN]);

// A signed message into out (>= OTAP_MSG_MAX); its length, 0 when the payload is too long. SET_KEY's payload is
// encrypted here
int otapBuild(const otapKeys_t *k, uint8_t cmd, uint32_t counter, uint32_t dst, uint32_t src, const uint8_t *payload,
	      uint8_t len, uint8_t *out);
// The header and lengths (no tag check): false when it isn't an OTAP message of ours
bool otapParse(const uint8_t *msg, int length, otapMsg_t *m);
// The tag checks out against the keys
bool otapVerify(const otapKeys_t *k, const uint8_t *msg, int length);
// SET_KEY's payload decrypted into out (m->len octets)
void otapDecrypt(const otapKeys_t *k, const uint8_t *msg, const otapMsg_t *m, uint8_t *out);

#endif
