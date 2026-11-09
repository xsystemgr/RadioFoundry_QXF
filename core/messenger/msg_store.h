// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// In-RAM message store for the analog messenger UI: an inbox, a sent box, a drafts box (all newest-first), and a HEARD
// table of the stations last decoded. Nothing here is serialized or written to flash - it lives only for the session,
// like the GOGUFW runtime HEARD list. The wire format still carries up to MSG_TEXT_LEN (64) bytes; the store keeps the
// shorter MSGR_TEXT_LEN so a modest number of messages fits in the RT-880's tight RAM.
#ifndef MSG_STORE_H
#define MSG_STORE_H

#include <stdint.h>
#include <stdbool.h>

#define MSGR_TEXT_LEN	32u	// stored/composed text cap (the on-air payload may be up to 64)
#define MSGR_CALL_LEN	8u	// callsign, matches the on-wire field width

#define MSGR_INBOX_MAX	6u
#define MSGR_SENT_MAX	6u
#define MSGR_DRAFT_MAX	4u
#define MSGR_HEARD_MAX	6u

typedef enum { MSG_BOX_INBOX = 0, MSG_BOX_SENT, MSG_BOX_DRAFTS } msg_box_t;

typedef struct {
	bool	used;
	uint8_t	type;			// MSG_PKT_TYPE_* (TEXT for drafts/sent)
	char	from[MSGR_CALL_LEN + 1];	// sender callsign (inbox); own call for sent; empty for drafts
	char	text[MSGR_TEXT_LEN + 1];
	bool	read;			// inbox only: has the user opened it
	uint16_t id;			// GGM2 message id (sent entries: to match an incoming ACK)
	bool	acked;			// sent only: an ACK for this id has come back (delivered)
} msg_entry_t;

typedef struct {
	bool	used;
	char	call[MSGR_CALL_LEN + 1];
	int8_t	rssi;			// dBm of the last packet heard from this station
	uint8_t	type;			// MSG_PKT_TYPE_* of the last packet
} msg_heard_t;

void msg_store_reset(void);

// Insert newest-first; the oldest entry is dropped when the box is full.
void msg_store_add_inbox(const char *from, const char *text, uint8_t type, int8_t rssi);
void msg_store_add_sent(const char *text, uint16_t id);	// id lets a later ACK mark it delivered
uint8_t msg_store_add_draft(const char *text);		// returns the new draft's index (0 = newest)
bool msg_store_ack(uint16_t id);			// mark the Sent entry with this id delivered; true if found
void msg_store_set_draft(uint8_t index, const char *text);

uint8_t msg_store_count(msg_box_t box);
const msg_entry_t *msg_store_get(msg_box_t box, uint8_t index);	// NULL if out of range
void msg_store_mark_read(msg_box_t box, uint8_t index);
void msg_store_delete(msg_box_t box, uint8_t index);
uint8_t msg_store_unread(void);				// unread inbox count (for a badge)

// HEARD table: upsert the station (moves it to the top), newest-first.
void msg_store_heard(const char *call, int8_t rssi, uint8_t type);
uint8_t msg_store_heard_count(void);
const msg_heard_t *msg_store_heard_get(uint8_t index);		// NULL if out of range

#endif
