// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Analog messenger UI (GOGUFW-compatible BK4819 FSK). A home menu (Inbox / Compose / Sent / Drafts / Heard / My call),
// a message-box list, a one-message read view, the HEARD list, and a self-contained keypad composer. Messages live in
// the RAM store (messenger/msg_store.h); the radio hooks (messenger/msg_radio.h) are implemented by the board. The whole
// file is gated on CONFIG_MESSENGER so it costs nothing in other builds.
#include "config.h"

typedef int msgr_tu_guard;	// keep this a non-empty translation unit when CONFIG_MESSENGER is off

#if CONFIG_MESSENGER
#include <string.h>
#include "ui_nav.h"
#include "ui_screens.h"
#include "fmt.h"
#include "hal_input.h"
#include "hal_time.h"
#include "messenger/msg_store.h"
#include "messenger/msg_packet.h"
#include "messenger/msg_radio.h"

// ---- shared state -------------------------------------------------------------------------------
static msg_box_t msgr_box;			// the box the list / read screens are showing
static char msgr_compose[MSGR_TEXT_LEN + 1];	// the message being composed
static char msgr_call[MSGR_CALL_LEN + 1] = "RT880";	// the local callsign (GGM2 "from")

static const char *type_short(uint8_t t)
{
	switch (t) {
	case MSG_PKT_TYPE_TEXT: return "MSG";
	case MSG_PKT_TYPE_ACK:  return "ACK";
	case MSG_PKT_TYPE_PING: return "PNG";
	case MSG_PKT_TYPE_PONG: return "PON";
	case MSG_PKT_TYPE_WAKE: return "WAK";
	default:                return "---";
	}
}

static const char *box_name(void)
{
	return msgr_box == MSG_BOX_INBOX ? "Inbox" : msgr_box == MSG_BOX_SENT ? "Sent" : "Drafts";
}

// ---- self-contained keypad composer (SCR_MSGR_COMPOSE) ------------------------------------------
// The same multi-tap scheme as the channel-name editor, kept here so the messenger needs none of the channel-editor
// machinery (which a lean messenger build leaves out). 2-9 cycle letters / digit / other-case; 1 punctuation; 0 space;
// # cycles ABC/123/abc; BACK or * delete, BACK on empty leaves; OK keeps and runs the done callback.
#define MSGR_MULTI_MS	1000u
static char *cmp_target;
static uint8_t cmp_max;
static void (*cmp_done)(void);
static char cmp_buf[MSGR_TEXT_LEN + 1];
static uint8_t cmp_len;
static int8_t cmp_key = -1;
static uint8_t cmp_idx;
static uint32_t cmp_ms;
enum { CMP_UPPER, CMP_DIGITS, CMP_LOWER, CMP_MODES };
static uint8_t cmp_mode;

static void msgr_edit_open(char *target, uint8_t max, void (*done)(void))
{
	cmp_target = target;
	cmp_max = max <= MSGR_TEXT_LEN ? max : MSGR_TEXT_LEN;
	cmp_done = done;
	ui_push(SCR_MSGR_COMPOSE, 0);
}

static char cmp_case(char c)
{
	return cmp_mode == CMP_LOWER && c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

static uint8_t cmp_cycle_len(const char *set)
{
	uint8_t n = (uint8_t)strlen(set);
	return set[0] >= 'A' && set[0] <= 'Z' ? (uint8_t)(2u * n - 1u) : n;
}

static char cmp_char(const char *set, uint8_t idx)
{
	uint8_t n = (uint8_t)strlen(set);
	if (!(set[0] >= 'A' && set[0] <= 'Z'))
		return set[idx];
	if (idx < n)
		return idx == n - 1u ? set[idx] : cmp_case(set[idx]);
	char c = set[idx - n];
	return cmp_mode == CMP_LOWER ? c : (char)(c - 'A' + 'a');
}

static void cmp_append(char c)
{
	if (cmp_len >= cmp_max) {
		ui_toast("That's the most it can hold");
		return;
	}
	cmp_buf[cmp_len++] = c;
	cmp_buf[cmp_len] = 0;
}

static void msgr_compose_enter(void)
{
	strncpy(cmp_buf, cmp_target, cmp_max);
	cmp_buf[cmp_max] = 0;
	cmp_len = (uint8_t)strlen(cmp_buf);
	cmp_key = -1;
	cmp_mode = CMP_UPPER;
}

static uint16_t msgr_compose_count(void) { return 1; }

static void msgr_compose_item(uint16_t i, ui_item_t *it)
{
	(void)i;
	sbuf_t b;
	sb_init(&b, it->label, sizeof it->label);
	sb_str(&b, cmp_buf);
	sb_chr(&b, '_');	// the cursor; the mode is in the title
	it->flags |= UI_ITEM_INFO;
}

static const char *msgr_compose_title(void)
{
	static const char *const modes[CMP_MODES] = { "ABC", "123", "abc" };
	return modes[cmp_mode];
}

static bool msgr_compose_key(const input_event_t *ev)
{
	static const char *const keys[10] = { " 0+#*", ".-/,'&()@_!?1", "ABC2", "DEF3", "GHI4", "JKL5", "MNO6",
					      "PQRS7", "TUV8", "WXYZ9" };
	bool press = ev->gesture == GESTURE_PRESS || ev->gesture == GESTURE_LONG;
	uint32_t now = hal_millis();
	if (ev->action >= ACT_DIGIT_0 && ev->action <= ACT_DIGIT_9 && press) {
		int8_t k = (int8_t)(ev->action - ACT_DIGIT_0);
		const char *set = keys[k];
		if (cmp_mode == CMP_DIGITS || ev->gesture == GESTURE_LONG) {
			cmp_append((char)('0' + k));
			cmp_key = -1;
		} else if (k == cmp_key && cmp_len > 0 && now - cmp_ms < MSGR_MULTI_MS) {
			cmp_idx = (uint8_t)((cmp_idx + 1u) % cmp_cycle_len(set));
			cmp_buf[cmp_len - 1] = cmp_char(set, cmp_idx);
			cmp_key = k;
		} else {
			cmp_idx = 0;
			cmp_append(cmp_char(set, 0));
			cmp_key = k;
		}
		cmp_ms = now;
		ui_dirty();
		return true;
	}
	int dir = ev->action == ACT_NAV_UP ? 1 : ev->action == ACT_NAV_DOWN ? -1 :
		ev->action == ACT_ENC_STEP ? (ev->value > 0 ? 1 : -1) : 0;
	if (dir != 0 && ev->gesture != GESTURE_UP) {
		if (cmp_len == 0)
			cmp_append('A');
		else if (dir < 0 && cmp_buf[cmp_len - 1] == ' ')
			cmp_buf[--cmp_len] = 0;
		else
			cmp_buf[cmp_len - 1] = (char)(' ' + (cmp_buf[cmp_len - 1] - ' ' + dir + 95) % 95);
		cmp_key = -1;
		ui_dirty();
		return true;
	}
	if (ev->action == ACT_CONTEXT && ev->gesture == GESTURE_PRESS) {
		cmp_mode = (uint8_t)((cmp_mode + 1u) % CMP_MODES);
		cmp_key = -1;
		ui_dirty();
		return true;
	}
	if (ev->action == ACT_BACK && ev->gesture == GESTURE_PRESS && cmp_len == 0) {
		ui_pop();
		return true;
	}
	if (((ev->action == ACT_BACK || ev->action == ACT_NAV_LEFT) && ev->gesture == GESTURE_PRESS) ||
	    (ev->action == ACT_QUICK && press)) {
		if (cmp_len > 0)
			cmp_buf[--cmp_len] = 0;
		cmp_key = -1;
		ui_dirty();
		return true;
	}
	if (ev->action == ACT_SELECT && press) {
		strncpy(cmp_target, cmp_buf, cmp_max);
		cmp_target[cmp_max] = 0;
		ui_pop();
		if (cmp_done != NULL)
			cmp_done();
		ui_dirty();
		return true;
	}
	return false;
}

const ui_scr_def_t msgr_compose_screen = {
	.name = "msgrcompose", .flags = SF_LIST, .prefix = "Write",
	.hint = "Back del  # ABC/123/abc  OK done",
	.count = msgr_compose_count, .item = msgr_compose_item, .key = msgr_compose_key, .enter = msgr_compose_enter,
	.title = msgr_compose_title,
};

// ---- compose/send completion callbacks ----------------------------------------------------------
static void msgr_send_done(void)
{
	if (msgr_compose[0] == '\0')
		return;			// nothing typed
	// The send is queued to the radio task; it lands in Sent when it keys, or in Drafts if the current frequency is
	// not a TX band. The UI just confirms it was accepted.
	if (msg_radio_send(msgr_compose))
		ui_toast("Sending...");
	else
		ui_toast("Busy - try again");
	msgr_compose[0] = '\0';
}

static void msgr_call_done(void)
{
	msg_radio_set_call(msgr_call);
	ui_toast("Callsign set");
}

// ---- home menu (SCR_MSGR) -----------------------------------------------------------------------
enum { MH_INBOX, MH_COMPOSE, MH_SENT, MH_DRAFTS, MH_HEARD, MH_CALL, MH_COUNT };

static void msgr_home_enter(void)
{
	msg_radio_set_call(msgr_call);
	msg_radio_rx_arm();		// keep listening while the messenger is open
}

static uint16_t msgr_home_count(void) { return MH_COUNT; }

static void msgr_home_item(uint16_t i, ui_item_t *it)
{
	sbuf_t b;
	sb_init(&b, it->value, sizeof it->value);
	switch (i) {
	case MH_INBOX: {
		uint8_t unread = msg_store_unread();
		strcpy(it->label, "Inbox");
		sb_u32(&b, msg_store_count(MSG_BOX_INBOX), 1);
		if (unread) {
			sb_str(&b, " (");
			sb_u32(&b, unread, 1);
			sb_str(&b, " new)");
			it->flags |= UI_ITEM_WARN;
		}
		break;
	}
	case MH_COMPOSE: strcpy(it->label, "Compose"); break;
	case MH_SENT:    strcpy(it->label, "Sent");   sb_u32(&b, msg_store_count(MSG_BOX_SENT), 1); break;
	case MH_DRAFTS:  strcpy(it->label, "Drafts"); sb_u32(&b, msg_store_count(MSG_BOX_DRAFTS), 1); break;
	case MH_HEARD:   strcpy(it->label, "Heard");  sb_u32(&b, msg_store_heard_count(), 1); break;
	case MH_CALL:    strcpy(it->label, "My call"); sb_str(&b, msgr_call); break;
	default: break;
	}
}

static void msgr_home_select(uint16_t i, bool hold)
{
	(void)hold;
	switch (i) {
	case MH_INBOX:  msgr_box = MSG_BOX_INBOX;  ui_push(SCR_MSGR_LIST, 0); break;
	case MH_COMPOSE: msgr_compose[0] = '\0';   msgr_edit_open(msgr_compose, MSGR_TEXT_LEN, msgr_send_done); break;
	case MH_SENT:   msgr_box = MSG_BOX_SENT;   ui_push(SCR_MSGR_LIST, 0); break;
	case MH_DRAFTS: msgr_box = MSG_BOX_DRAFTS; ui_push(SCR_MSGR_LIST, 0); break;
	case MH_HEARD:  ui_push(SCR_MSGR_HEARD, 0); break;
	case MH_CALL:   msgr_edit_open(msgr_call, MSGR_CALL_LEN, msgr_call_done); break;
	default: break;
	}
}

const ui_scr_def_t msgr_home_screen = {
	.name = "msgr", .flags = SF_LIST | SF_FIELD, .prefix = "Messenger", .hint = "OK open  Back",
	.count = msgr_home_count, .item = msgr_home_item, .select = msgr_home_select, .enter = msgr_home_enter,
};

// ---- message box list (SCR_MSGR_LIST) -----------------------------------------------------------
static uint16_t msgr_list_count(void) { return msg_store_count(msgr_box); }

static void msgr_list_item(uint16_t i, ui_item_t *it)
{
	const msg_entry_t *e = msg_store_get(msgr_box, (uint8_t)i);
	if (e == NULL)
		return;
	sbuf_t b;
	sb_init(&b, it->label, sizeof it->label);
	if (msgr_box == MSG_BOX_INBOX) {
		sb_str(&b, e->from[0] ? e->from : "?");
		if (!e->read)
			it->flags |= UI_ITEM_WARN;		// unread stands out
	} else {
		sb_str(&b, "ALL");
	}
	sbuf_t v;
	sb_init(&v, it->value, sizeof it->value);
	sb_str(&v, e->text);
	it->flags |= UI_ITEM_PREVIEW;			// the text is shortened at its end, not the middle
}

static void msgr_list_select(uint16_t i, bool hold)
{
	(void)hold;
	if (msgr_box == MSG_BOX_DRAFTS) {
		const msg_entry_t *e = msg_store_get(MSG_BOX_DRAFTS, (uint8_t)i);
		if (e != NULL) {
			strncpy(msgr_compose, e->text, MSGR_TEXT_LEN);
			msgr_compose[MSGR_TEXT_LEN] = 0;
			msgr_edit_open(msgr_compose, MSGR_TEXT_LEN, msgr_send_done);	// edit then (re)send
		}
		return;
	}
	ui_push(SCR_MSGR_READ, i);
}

static void msgr_list_context(uint16_t i)
{
	msg_store_delete(msgr_box, (uint8_t)i);
	ui_toast("Deleted");
	ui_dirty();
}

static const char *msgr_list_title(void) { return box_name(); }

const ui_scr_def_t msgr_list_screen = {
	.name = "msgrlist", .flags = SF_LIST | SF_FIELD, .hint = "OK read  CONTEXT del  Back", .empty = "Empty",
	.count = msgr_list_count, .item = msgr_list_item, .select = msgr_list_select, .context = msgr_list_context,
	.title = msgr_list_title,
};

// ---- read one message (SCR_MSGR_READ, arg = index) ----------------------------------------------
static void msgr_read_enter(void)
{
	msg_store_mark_read(msgr_box, (uint8_t)ui_frame()->arg);
}

static uint16_t msgr_read_count(void)
{
	return msgr_box == MSG_BOX_INBOX ? 2u : 1u;	// inbox: Reply + Delete; sent/drafts: Delete
}

static void msgr_read_item(uint16_t i, ui_item_t *it)
{
	if (msgr_box == MSG_BOX_INBOX)
		strcpy(it->label, i == 0 ? "Reply" : "Delete");
	else
		strcpy(it->label, "Delete");
}

static void msgr_read_select(uint16_t i, bool hold)
{
	(void)hold;
	if (msgr_box == MSG_BOX_INBOX && i == 0) {
		msgr_compose[0] = '\0';
		msgr_edit_open(msgr_compose, MSGR_TEXT_LEN, msgr_send_done);
		return;
	}
	msg_store_delete(msgr_box, (uint8_t)ui_frame()->arg);
	ui_pop();
	ui_toast("Deleted");
}

static const char *msgr_read_body(void)
{
	static char t[MSGR_TEXT_LEN + 40];
	const msg_entry_t *e = msg_store_get(msgr_box, (uint8_t)ui_frame()->arg);
	if (e == NULL)
		return "Deleted";
	sbuf_t b;
	sb_init(&b, t, sizeof t);
	if (msgr_box == MSG_BOX_INBOX) {
		sb_str(&b, "From ");
		sb_str(&b, e->from[0] ? e->from : "?");
		sb_str(&b, " [");
		sb_str(&b, type_short(e->type));
		sb_chr(&b, ']');
	} else {
		sb_str(&b, "To ALL");
	}
	sb_chr(&b, '\n');
	sb_str(&b, e->text);
	return t;
}

static const char *msgr_read_title(void) { return box_name(); }

const ui_scr_def_t msgr_read_screen = {
	.name = "msgrread", .flags = SF_LIST | SF_FIELD, .hint = "OK choose  Back",
	.count = msgr_read_count, .item = msgr_read_item, .select = msgr_read_select, .body = msgr_read_body,
	.title = msgr_read_title, .enter = msgr_read_enter,
};

// ---- HEARD list (SCR_MSGR_HEARD) ----------------------------------------------------------------
static uint16_t msgr_heard_count(void) { return msg_store_heard_count(); }

static void msgr_heard_item(uint16_t i, ui_item_t *it)
{
	const msg_heard_t *h = msg_store_heard_get((uint8_t)i);
	if (h == NULL)
		return;
	strcpy(it->label, h->call);
	sbuf_t b;
	sb_init(&b, it->value, sizeof it->value);
	if (h->rssi < 0) {
		sb_chr(&b, '-');
		sb_u32(&b, (uint32_t)(-(int)h->rssi), 1);
	} else {
		sb_u32(&b, (uint32_t)h->rssi, 1);
	}
	sb_str(&b, "dBm ");
	sb_str(&b, type_short(h->type));
}

const ui_scr_def_t msgr_heard_screen = {
	.name = "msgrheard", .flags = SF_LIST | SF_FIELD, .prefix = "Heard", .hint = "Back",
	.empty = "Nothing heard yet",
	.count = msgr_heard_count, .item = msgr_heard_item,
};

#endif /* CONFIG_MESSENGER */
