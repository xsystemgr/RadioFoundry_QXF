// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// In-RAM message store (msg_store.h). Each box is a small newest-first array; inserting at the front shifts the rest
// down and drops the oldest when full. No dynamic allocation, no flash.
#include <string.h>
#include "messenger/msg_store.h"
#include "messenger/msg_packet.h"

static msg_entry_t inbox[MSGR_INBOX_MAX];
static msg_entry_t sent[MSGR_SENT_MAX];
static msg_entry_t drafts[MSGR_DRAFT_MAX];
static msg_heard_t heard[MSGR_HEARD_MAX];

static msg_entry_t *box_base(msg_box_t box, uint8_t *cap)
{
	switch (box) {
	case MSG_BOX_INBOX:  *cap = MSGR_INBOX_MAX; return inbox;
	case MSG_BOX_SENT:   *cap = MSGR_SENT_MAX;  return sent;
	case MSG_BOX_DRAFTS: *cap = MSGR_DRAFT_MAX; return drafts;
	default:             *cap = 0;              return NULL;
	}
}

static void copy_field(char *dst, const char *src, uint8_t cap)
{
	uint8_t i = 0;
	for (; src != NULL && src[i] != '\0' && i < cap; i++) {
		dst[i] = src[i];
	}
	dst[i] = '\0';
}

void msg_store_reset(void)
{
	memset(inbox, 0, sizeof inbox);
	memset(sent, 0, sizeof sent);
	memset(drafts, 0, sizeof drafts);
	memset(heard, 0, sizeof heard);
}

uint8_t msg_store_count(msg_box_t box)
{
	uint8_t cap;
	const msg_entry_t *b = box_base(box, &cap);
	uint8_t n = 0;
	for (uint8_t i = 0; b != NULL && i < cap; i++) {
		if (b[i].used) n++;
	}
	return n;
}

const msg_entry_t *msg_store_get(msg_box_t box, uint8_t index)
{
	uint8_t cap;
	const msg_entry_t *b = box_base(box, &cap);
	if (b == NULL || index >= cap || !b[index].used) {
		return NULL;
	}
	return &b[index];
}

// Shift the box down by one (dropping the last) and return the now-free slot 0 for the newest entry.
static msg_entry_t *box_push_front(msg_box_t box)
{
	uint8_t cap;
	msg_entry_t *b = box_base(box, &cap);
	if (b == NULL || cap == 0) {
		return NULL;
	}
	for (uint8_t i = cap - 1u; i > 0u; i--) {
		b[i] = b[i - 1u];
	}
	memset(&b[0], 0, sizeof b[0]);
	b[0].used = true;
	return &b[0];
}

void msg_store_add_inbox(const char *from, const char *text, uint8_t type, int8_t rssi)
{
	msg_entry_t *e = box_push_front(MSG_BOX_INBOX);
	if (e == NULL) return;
	e->type = type;
	e->read = false;
	copy_field(e->from, from, MSGR_CALL_LEN);
	copy_field(e->text, text, MSGR_TEXT_LEN);
	msg_store_heard(from, rssi, type);
}

void msg_store_add_sent(const char *text, uint16_t id)
{
	msg_entry_t *e = box_push_front(MSG_BOX_SENT);
	if (e == NULL) return;
	e->type = MSG_PKT_TYPE_TEXT;
	e->read = true;
	e->from[0] = '\0';
	e->id = id;
	e->acked = false;
	copy_field(e->text, text, MSGR_TEXT_LEN);
}

bool msg_store_ack(uint16_t id)
{
	for (uint8_t i = 0; i < MSGR_SENT_MAX; i++) {
		if (sent[i].used && sent[i].id == id) {
			sent[i].acked = true;
			return true;
		}
	}
	return false;
}

uint8_t msg_store_add_draft(const char *text)
{
	msg_entry_t *e = box_push_front(MSG_BOX_DRAFTS);
	if (e == NULL) return 0;
	e->type = MSG_PKT_TYPE_TEXT;
	e->read = true;
	e->from[0] = '\0';
	copy_field(e->text, text, MSGR_TEXT_LEN);
	return 0;	// always inserted at the front
}

void msg_store_set_draft(uint8_t index, const char *text)
{
	if (index >= MSGR_DRAFT_MAX || !drafts[index].used) return;
	copy_field(drafts[index].text, text, MSGR_TEXT_LEN);
}

void msg_store_mark_read(msg_box_t box, uint8_t index)
{
	uint8_t cap;
	msg_entry_t *b = box_base(box, &cap);
	if (b != NULL && index < cap && b[index].used) {
		b[index].read = true;
	}
}

void msg_store_delete(msg_box_t box, uint8_t index)
{
	uint8_t cap;
	msg_entry_t *b = box_base(box, &cap);
	if (b == NULL || index >= cap || !b[index].used) {
		return;
	}
	for (uint8_t i = index; i + 1u < cap; i++) {
		b[i] = b[i + 1u];
	}
	memset(&b[cap - 1u], 0, sizeof b[cap - 1u]);
}

uint8_t msg_store_unread(void)
{
	uint8_t n = 0;
	for (uint8_t i = 0; i < MSGR_INBOX_MAX; i++) {
		if (inbox[i].used && !inbox[i].read) n++;
	}
	return n;
}

void msg_store_heard(const char *call, int8_t rssi, uint8_t type)
{
	if (call == NULL || call[0] == '\0') {
		return;
	}
	// If the station is already in the table, drop its old row first so the upsert lands at the top.
	for (uint8_t i = 0; i < MSGR_HEARD_MAX; i++) {
		if (heard[i].used && strncmp(heard[i].call, call, MSGR_CALL_LEN) == 0) {
			for (uint8_t j = i; j + 1u < MSGR_HEARD_MAX; j++) {
				heard[j] = heard[j + 1u];
			}
			memset(&heard[MSGR_HEARD_MAX - 1u], 0, sizeof heard[0]);
			break;
		}
	}
	for (uint8_t i = MSGR_HEARD_MAX - 1u; i > 0u; i--) {
		heard[i] = heard[i - 1u];
	}
	memset(&heard[0], 0, sizeof heard[0]);
	heard[0].used = true;
	copy_field(heard[0].call, call, MSGR_CALL_LEN);
	heard[0].rssi = rssi;
	heard[0].type = type;
}

uint8_t msg_store_heard_count(void)
{
	uint8_t n = 0;
	for (uint8_t i = 0; i < MSGR_HEARD_MAX; i++) {
		if (heard[i].used) n++;
	}
	return n;
}

const msg_heard_t *msg_store_heard_get(uint8_t index)
{
	if (index >= MSGR_HEARD_MAX || !heard[index].used) {
		return NULL;
	}
	return &heard[index];
}
