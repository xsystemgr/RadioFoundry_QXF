// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
#include <assert.h>
#include <stdio.h>
#include "board.h"
#include "platform.h"
#include "../radio.h"
#include "input/input_engine.h"

static unsigned writes;
static unsigned active_row = 99;
static unsigned pressed = 99;
static const unsigned rows[] = { 10, 2, 1, 0 };
static bool pins[6][16];
static uint8_t columns[4], lines[4];
static unsigned parameter;
static unsigned serial_bits, command_count, data_count, pixel_count;
static uint8_t serial_byte, last_command, first_pixel, last_data, orientation;
void at32_port_write(uint32_t port, uint32_t mask)
{
	assert((mask & ~( (1u << 3) | (1u << 5) | (1u << 19) | (1u << 21))) == 0);
	assert((mask & (mask >> 16) & 0xFFFFu) == 0);
	for (unsigned pin = 0; pin < 16; pin++) {
		if (mask & (1u << (pin + 16))) at32_pin_write(port, pin, false);
		if (mask & (1u << pin)) at32_pin_write(port, pin, true);
	}
}
void at32_pin_output(uint32_t port, unsigned pin, bool level) { at32_pin_write(port, pin, level); }
uint32_t at32_irq_save(void) { return 0; }
void at32_irq_restore(uint32_t state) { (void)state; }
void at32_pin_input_floating(uint32_t port, unsigned pin) { (void)port; (void)pin; }
void at32_pin_input(uint32_t port, unsigned pin) { (void)port; (void)pin; }
void at32_pin_write(uint32_t port, unsigned pin, bool level)
{
	writes++;
	unsigned bank = (port - AT32_GPIO_A) / 0x400;
	bool old = pins[bank][pin];
	pins[bank][pin] = level;
	if (port == AT32_GPIO_B && pin == 3 && !old && level && !pins[0][15]) {
		serial_byte = (uint8_t)((serial_byte << 1) | pins[1][5]);
		if (++serial_bits == 8) {
			if (pins[2][13]) {
				data_count++;
				if (last_command == 0x2A) { assert(parameter < 4); columns[parameter++] = serial_byte; }
				if (last_command == 0x2B) { assert(parameter < 4); lines[parameter++] = serial_byte; }
				last_data = serial_byte;
				if (last_command == 0x36) orientation = serial_byte;
				if (last_command == 0x2C && pixel_count++ == 0) first_pixel = serial_byte;
			} else {
				command_count++;
				last_command = serial_byte;
				parameter = 0;
			}
			serial_bits = 0;
		}
	}
	if (port != AT32_GPIO_B) return;
	for (unsigned i = 0; i < 4; i++) if (pin == rows[i]) active_row = level ? i : 99;
}
bool at32_pin_read(uint32_t port, unsigned pin)
{
	unsigned col = port == AT32_GPIO_D ? 3 : pin - 10;
	return pressed < 16 && active_row < 4 && pressed == active_row * 4 + col;
}
void hal_delay_us(uint32_t value) { (void)value; }
void hal_delay_ms(uint32_t value) { (void)value; }
static unsigned event_count, repeat_count;
static input_event_t last_event;
static void emit(const input_event_t *event, void *ctx)
{
	(void)ctx;
	event_count++;
	if (event->gesture == GESTURE_REPEAT) repeat_count++;
	last_event = *event;
}
static void check_key_actions(void)
{
	static const uint8_t expected[] = {
		ACT_DIGIT_1, ACT_DIGIT_4, ACT_DIGIT_7, ACT_QUICK,
		ACT_DIGIT_2, ACT_DIGIT_5, ACT_DIGIT_8, ACT_DIGIT_0,
		ACT_DIGIT_3, ACT_DIGIT_6, ACT_DIGIT_9, ACT_VFO_TOGGLE,
		ACT_SELECT, ACT_NAV_UP, ACT_NAV_DOWN, ACT_BACK,
	};
	for (unsigned key = 0; key < 16; key++) {
		input_engine_t engine;
		input_engine_init(&engine, board.keymap, board.keymap_len, 0);
		event_count = repeat_count = 0;
		pressed = 99;
		input_engine_step(&engine, board.input->scan(), 0, 10, emit, NULL);
		pressed = key;
		for (unsigned t = 20; t <= 50; t += 10)
			input_engine_step(&engine, board.input->scan(), 0, t, emit, NULL);
		pressed = 99;
		for (unsigned t = 60; t <= 90; t += 10)
			input_engine_step(&engine, board.input->scan(), 0, t, emit, NULL);
		assert(event_count == 1 && last_event.action == expected[key]);
		assert(last_event.gesture == GESTURE_PRESS);
	}
	// Held arrows repeat the same direction; * reaches the core's lock gesture.
	const unsigned held_keys[] = { 13, 14, 3, 15 };
	for (unsigned i = 0; i < sizeof(held_keys)/sizeof(held_keys[0]); i++) {
		input_engine_t engine;
		input_engine_init(&engine, board.keymap, board.keymap_len, 0);
		event_count = repeat_count = 0;
		pressed = 99;
		input_engine_step(&engine, 0, 0, 10, emit, NULL);
		pressed = held_keys[i];
		for (unsigned t = 20; t <= 1200; t += 10)
			input_engine_step(&engine, board.input->scan(), 0, t, emit, NULL);
		assert(event_count > 0 && last_event.action == expected[held_keys[i]]);
		assert(i < 2 ? repeat_count > 0 : last_event.gesture == GESTURE_LONG);
	}
	pressed = 99;
}
int main(void)
{
	for (uint32_t hz = 1000000; hz <= 1000000000; hz += 1000000) {
		assert(!board.radio->tx(hz, RADIO_BW_25K, 255, NULL));
		assert(!board.radio->rx(hz, RADIO_BW_12K5, NULL));
	}
	assert(writes == 0);
	// The air band (first entry) is RX-only; the VHF and UHF ranges allow TX.
	assert(!board.bands[0].tx && board.bands[1].tx && board.bands[2].tx);
	unsigned digits = 0;
	for (unsigned i = 0; i < board.keymap_len; i++) {
		assert(board.keymap[i].short_action != ACT_PTT);
		assert(board.keymap[i].long_action != ACT_PTT);
		unsigned action = board.keymap[i].short_action;
		if (action >= ACT_DIGIT_0 && action <= ACT_DIGIT_9) digits |= 1u << (action - ACT_DIGIT_0);
	}
	assert(digits == 0x3FFu);
	assert(board.keymap[11].short_action == ACT_VFO_TOGGLE);
	assert(!board.flash->init());
	assert(!board.flash->write(0, "x", 1));
	assert(!board.flash->erase(0));
	assert(board.flash->map == NULL && board.flash->size == 0);
	assert(writes == 0);
	board.input->init();
	for (pressed = 0; pressed < 16; pressed++) {
		assert(board.input->scan() == (1u << pressed));
		assert(active_row == 99);
	}
	pressed = 99;
	assert(board.input->scan() == 0);
	check_key_actions();
	board_early_init();
	board.display->init();
	assert(serial_bits == 0 && last_command == 0x29);
	assert(orientation == 0x00);
	assert(pins[0][15] && pins[2][7] && pins[0][8] && pins[1][12]);
	static uint8_t frame[9600];
	for (unsigned i = 0; i < sizeof(frame); i++) frame[i] = 0xA5;
	uint16_t palette[16] = {0};
	palette[10] = 0x1234; palette[5] = 0x5678;
	command_count = data_count = pixel_count = 0;
	board.display->flush_indexed(frame, palette, 0, 1);
	assert(command_count == 3 && last_command == 0x2C);
	assert(board.display->width == 120 && board.display->height == 160);
	assert(data_count == 968 && pixel_count == 960);
	assert(first_pixel == 0x12 && last_data == 0x78 && serial_bits == 0);
	assert(pins[0][15]);
	assert(columns[0] == 0 && columns[1] == 0 && columns[2] == 0 && columns[3] == 239);
	assert(lines[0] == 0 && lines[1] == 0 && lines[2] == 0 && lines[3] == 1);
	command_count = data_count = pixel_count = 0;
	board.display->flush_indexed(frame, palette, 159, 160);
	assert(command_count == 3 && data_count == 968 && pixel_count == 960);
	assert(first_pixel == 0x12 && last_data == 0x78 && pins[0][15]);
	assert(columns[0] == 0 && columns[1] == 0 && columns[2] == 0 && columns[3] == 239);
	assert(lines[0] == 1 && lines[1] == 62 && lines[2] == 1 && lines[3] == 63);
	puts("board-test: TX/storage denied; matrix/key legends/gestures and serial LCD/scale/byte order passed");
	return 0;
}
