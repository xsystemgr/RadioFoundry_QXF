// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Closed-lab UI bring-up. Wiring is software-observed, not bench-verified.
#include <stddef.h>
#include <stdio.h>
#include "board.h"
#include "platform.h"
#include "hal_time.h"
#include "display/st7789/st7789.h"

static void nothing(void) {}
static void ignore_bool(bool on) { (void)on; }

// Deassert both observed RF selects. This does not identify or disable an external PA.
void board_rf_safe(void)
{
	at32_pin_output(AT32_GPIO_C, 7, true);
	at32_pin_output(AT32_GPIO_A, 8, true);
}
void board_early_init(void)
{
	board_rf_safe();
	at32_pin_output(AT32_GPIO_B, 12, true); // NOR stays deselected; no flash driver
	at32_pin_output(AT32_GPIO_A, 9, false);
}
void board_init(void) {}

static void lcd_byte(uint8_t value)
{
	for (unsigned bit = 0; bit < 8; bit++) {
		at32_pin_write(AT32_GPIO_B, 3, false);
		at32_pin_write(AT32_GPIO_B, 5, (value & 0x80u) != 0);
		hal_delay_us(1);
		at32_pin_write(AT32_GPIO_B, 3, true);
		hal_delay_us(1);
		value <<= 1;
	}
}
static void lcd_begin(void) { at32_pin_write(AT32_GPIO_A, 15, false); }
static void lcd_end(void) { at32_pin_write(AT32_GPIO_A, 15, true); }
static void lcd_cmd(uint8_t value)
{
	at32_pin_write(AT32_GPIO_C, 13, false);
	lcd_byte(value);
}
static void lcd_data(const uint8_t *data, uint32_t count)
{
	at32_pin_write(AT32_GPIO_C, 13, true);
	while (count--) lcd_byte(*data++);
}
static void lcd_pixels(const uint16_t *pixels, uint32_t count)
{
	at32_pin_write(AT32_GPIO_C, 13, true);
	while (count--) {
		uint16_t value = *pixels++;
		lcd_byte((uint8_t)(value >> 8));
		lcd_byte((uint8_t)value);
	}
}
static void lcd_reset(bool level) { at32_pin_write(AT32_GPIO_C, 14, level); }
static const st7789_bus_t bus = {
	.begin = lcd_begin, .end = lcd_end, .cmd = lcd_cmd, .data = lcd_data,
	.pixels = lcd_pixels, .reset = lcd_reset, .delay_ms = hal_delay_ms,
};
// Reuse the existing 160x120 UI geometry: landscape, doubled to 320x240.
static const st7789_t panel = {
	.bus = &bus, .width = 320, .height = 240, .madctl = 0x60, .scale = 2,
};
static void display_init(void)
{
	at32_pin_output(AT32_GPIO_A, 15, true);
	at32_pin_output(AT32_GPIO_B, 3, true);
	at32_pin_output(AT32_GPIO_B, 5, false);
	at32_pin_output(AT32_GPIO_C, 13, false);
	at32_pin_output(AT32_GPIO_C, 14, true);
	st7789_init(&panel);
}
static void display_flush(const void *fb, const uint16_t *palette, uint16_t y0, uint16_t y1)
{
	st7789_write_rows_indexed(&panel, fb, palette, y0, y1);
}
static void backlight(uint8_t percent) { at32_pin_write(AT32_GPIO_A, 9, percent != 0); }
static const hal_display_t display = {
	.width = 160, .height = 120, .bpp = 4, .init = display_init,
	.flush_indexed = display_flush, .set_backlight = backlight,
};

static const uint8_t rows[] = { 10, 2, 1, 0 };
static uint32_t last_keys;
static void input_init(void)
{
	for (unsigned i = 0; i < 4; i++) at32_pin_output(AT32_GPIO_B, rows[i], false);
	for (unsigned i = 10; i <= 12; i++) at32_pin_input(AT32_GPIO_C, i);
	at32_pin_input(AT32_GPIO_D, 2);
}
static uint32_t input_scan(void)
{
	uint32_t result = 0;
	for (unsigned row = 0; row < 4; row++) {
		at32_pin_write(AT32_GPIO_B, rows[row], true);
		hal_delay_us(5);
		for (unsigned col = 0; col < 4; col++) {
			bool high = col == 3 ? at32_pin_read(AT32_GPIO_D, 2)
				: at32_pin_read(AT32_GPIO_C, 10 + col);
			if (high) result |= 1u << (row * 4 + col);
		}
		at32_pin_write(AT32_GPIO_B, rows[row], false);
	}
	last_keys = result;
	return result;
}
static int8_t encoder(void) { return 0; }
static const hal_input_t input = { .init = input_init, .scan = input_scan, .encoder = encoder };
// Temporary positional controls, not claims about the printed key legends. No PTT binding.
static const keymap_entry_t keys[] = {
	{ .bit = 0, .short_action = ACT_NAV_UP, .repeat = true },
	{ .bit = 1, .short_action = ACT_NAV_DOWN, .repeat = true },
	{ .bit = 2, .short_action = ACT_SELECT },
	{ .bit = 3, .short_action = ACT_BACK },
};

static void set_mode(radio_mode_t mode) { (void)mode; }
static bool receive(uint32_t hz, radio_bw_t bw, const subtone_t *tone)
{
	(void)hz; (void)bw; (void)tone; return false;
}
static bool transmit(uint32_t hz, radio_bw_t bw, uint8_t power, const subtone_t *tone)
{
	(void)hz; (void)bw; (void)power; (void)tone; return false;
}
static void status(radio_status_t *value)
{
	*value = (radio_status_t){ .rssi_dbm = -127, .noise = 255 };
}
static const hal_radio_t radio = {
	.init = nothing, .set_mode = set_mode, .rx = receive, .tx = transmit,
	.tx_off = nothing, .status = status, .audio = ignore_bool,
};
static bool flash_init(void) { return false; }
static bool flash_read(uint32_t address, void *buf, uint32_t len)
{
	(void)address; (void)buf; (void)len; return false;
}
static bool flash_write(uint32_t address, const void *buf, uint32_t len)
{
	(void)address; (void)buf; (void)len; return false;
}
static bool flash_erase(uint32_t address) { (void)address; return false; }
static const hal_flash_t flash = {
	.init = flash_init, .read = flash_read, .write = flash_write, .erase = flash_erase,
};
static uint16_t battery_mv(void) { return 0; }
static uint8_t battery_pct(void) { return 0; }
static int8_t volume(void) { return -1; }
static bool switch_on(void) { return true; }
static void led(uint8_t which, bool on) { (void)which; (void)on; }
static const hal_power_t power = {
	.init = nothing, .latch = ignore_bool, .battery_mv = battery_mv, .battery_pct = battery_pct,
	.volume = volume, .switch_on = switch_on, .led = led,
};
static bool diag(uint8_t line, char *buf, uint8_t len)
{
	switch (line) {
	case 0: snprintf(buf, len, "UI ONLY - RF DISABLED"); break;
	case 1: snprintf(buf, len, "Keys: %04lX", (unsigned long)last_keys); break;
	case 2: snprintf(buf, len, "No storage writes"); break;
	case 3: snprintf(buf, len, "MCU/PA not verified"); break;
	default: return false;
	}
	return true;
}
static const band_t bands[] = { { 136000000, 174000000, false }, { 400000000, 480000000, false } };
static const char *const power_names[] = { "OFF" };
const board_t board = {
	.name = "RT-880 LAB NO RF", .caps = CAP_KEYPAD,
	.bands = bands, .band_count = 2, .power_count = 1, .power_names = power_names,
	.display = &display, .input = &input, .keymap = keys, .keymap_len = 4,
	.radio = &radio, .power = &power, .flash = &flash, .diag_line = diag,
};
