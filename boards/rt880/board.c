// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Closed-lab UI bring-up. Wiring is software-observed, not bench-verified.
#include <stddef.h>
#include <stdio.h>
#include "board.h"
#include "config.h"
#include "radio.h"
#include "platform.h"
#include "hal_time.h"
#include "display/st7789/st7789.h"
#if CONFIG_BCAST_FM
#include "bcast/si4732/si4732_hal.h"
#endif

static void nothing(void) {}
static void ignore_bool(bool on) { (void)on; }

void board_rf_safe(void) { rt880_rf_halt(); }
void board_early_init(void)
{
	board_rf_safe();
	at32_pin_output(AT32_GPIO_B, 12, true); // NOR stays deselected; no flash driver
	at32_pin_output(AT32_GPIO_A, 9, false);
}

#if CONFIG_BCAST_FM
// Si4732 HF/AM receiver, software I2C on PC0 (SCL) / PC1 (SDA), power on PC5, 7-bit address 0x11 (SEN low).
// SDA is open drain in software: released = input with its pull-up, low = driven output low. SCL is push-pull.
#define SI4732_SCL_PIN	0u
#define SI4732_SDA_PIN	1u
#define SI4732_PWR_PIN	5u

static bool si_pins_ready;
static void si_pins_setup(void)
{
	if (si_pins_ready) {
		return;
	}
	at32_pin_output(AT32_GPIO_C, SI4732_PWR_PIN, false);	// power off until enable()
	at32_pin_output(AT32_GPIO_C, SI4732_SCL_PIN, true);	// SCL idle high
	at32_pin_input_pullup(AT32_GPIO_C, SI4732_SDA_PIN);	// SDA released
	si_pins_ready = true;
}
static void si_scl(bool high) { at32_pin_write(AT32_GPIO_C, SI4732_SCL_PIN, high); }
static void si_sda(bool high)
{
	if (high) {
		at32_pin_input_pullup(AT32_GPIO_C, SI4732_SDA_PIN);	// release the line
	} else {
		at32_pin_output(AT32_GPIO_C, SI4732_SDA_PIN, false);	// drive it low
	}
}
static bool si_sda_in(void) { return at32_pin_read(AT32_GPIO_C, SI4732_SDA_PIN); }
static const i2c_gpio_t si_bus = {
	.scl = si_scl, .sda = si_sda, .sda_in = si_sda_in, .delay_us = hal_delay_us, .half_us = 5,
};
static void si_enable(bool on)
{
	si_pins_setup();
	at32_pin_write(AT32_GPIO_C, SI4732_PWR_PIN, on);
}
// The tuner's analog audio shares the one speaker amplifier with the channel receiver. The core drives the two audio
// paths in turn, so the speaker gate (PF8/PC4) is owned jointly in radio.c: here we only announce whether the
// broadcast path wants it. radio.c keeps the gate open while either receiver asks.
static void si_audio(bool on)
{
	rt880_bcast_speaker(on);
}
static const si4732_hal_cfg_t bcast_cfg = {
	.bus = &si_bus, .addr7 = 0x11u, .delay_ms = hal_delay_ms,
	.enable = si_enable, .audio_path = si_audio, .start_khz = 7100u,
};
void board_init(void) { extern const hal_flash_t rt880_flash; rt880_flash.init(); si4732_hal_init(&bcast_cfg); }
#else
void board_init(void) { extern const hal_flash_t rt880_flash; rt880_flash.init(); }
#endif

static void lcd_byte(uint8_t value)
{
	// Lower clock and set data in one atomic write; leave all other pins alone.
	// Unroll at the fixed 48 MHz clock; retain the RF bus delays separately.
#pragma GCC unroll 8
	for (unsigned bit = 0; bit < 8; bit++) {
		uint32_t data = (value & 0x80u) ? (1u << 5) : (1u << 21);
		at32_port_write(AT32_GPIO_B, (1u << 19) | data);
		at32_port_write(AT32_GPIO_B, 1u << 3);
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
// Portrait UI, 120x160 indexed pixels doubled to the native 240x320 panel.
static const st7789_t panel = {
	.bus = &bus, .width = 240, .height = 320, .madctl = 0x00, .scale = 2,
};
static void display_init(void)
{
	// Enable before reset/commands: the pin's role may include panel power.
	at32_pin_output(AT32_GPIO_A, 9, true);
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
	.width = 120, .height = 160, .bpp = 4, .init = display_init,
	.flush_indexed = display_flush, .set_backlight = backlight,
};

static const uint8_t rows[] = { 10, 2, 1, 0 };
static uint32_t last_keys;
#define PTT_BIT	16u	// virtual scan bit for the PTT (outside the 4x4 matrix, bits 0-15)
static void input_init(void)
{
	for (unsigned i = 0; i < 4; i++) at32_pin_output(AT32_GPIO_B, rows[i], false);
	for (unsigned i = 10; i <= 12; i++) at32_pin_input(AT32_GPIO_C, i);
	at32_pin_input(AT32_GPIO_D, 2);
	at32_pin_input(AT32_GPIO_B, 6);		// PTT, active low (not part of the keypad matrix)
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
	if (!at32_pin_read(AT32_GPIO_B, 6))	// PTT pressed = PB6 low
		result |= 1u << PTT_BIT;
	last_keys = result;
	return result;
}
static int8_t encoder(void) { return 0; }
static const hal_input_t input = { .init = input_init, .scan = input_scan, .encoder = encoder };
// Matrix coordinates follow electrical scan order, not the front-panel rows.
// Front legends inferred from the reference key translation; no PTT binding.
static const keymap_entry_t keys[] = {
	{ .bit = 0, .short_action = ACT_DIGIT_1, .long_action = ACT_DIGIT_1 },
	{ .bit = 1, .short_action = ACT_DIGIT_4, .long_action = ACT_DIGIT_4 },
	{ .bit = 2, .short_action = ACT_DIGIT_7, .long_action = ACT_DIGIT_7 },
	{ .bit = 3, .short_action = ACT_QUICK, .long_action = ACT_QUICK, .dtmf = '*' },	// hold *: core key-lock gesture
	{ .bit = 4, .short_action = ACT_DIGIT_2, .long_action = ACT_DIGIT_2 },
	{ .bit = 5, .short_action = ACT_DIGIT_5, .long_action = ACT_DIGIT_5 },
	{ .bit = 6, .short_action = ACT_DIGIT_8, .long_action = ACT_DIGIT_8 },
	{ .bit = 7, .short_action = ACT_DIGIT_0, .long_action = ACT_DIGIT_0 },
	{ .bit = 8, .short_action = ACT_DIGIT_3, .long_action = ACT_DIGIT_3 },
	{ .bit = 9, .short_action = ACT_DIGIT_6, .long_action = ACT_DIGIT_6 },
	{ .bit = 10, .short_action = ACT_DIGIT_9, .long_action = ACT_DIGIT_9 },
	{ .bit = 11, .short_action = ACT_VFO_TOGGLE, .long_action = ACT_AB, .dtmf = '#' },	// #: VFO/MR; hold #: switch A/B VFO
	{ .bit = 12, .short_action = ACT_SELECT, .long_action = ACT_SELECT },
	{ .bit = 13, .short_action = ACT_NAV_UP, .long_action = ACT_NAV_UP, .repeat = true },
	{ .bit = 14, .short_action = ACT_NAV_DOWN, .long_action = ACT_NAV_DOWN, .repeat = true },
	{ .bit = 15, .short_action = ACT_BACK, .long_action = ACT_BACK },
	{ .bit = PTT_BIT, .short_action = ACT_PTT, .long_action = ACT_PTT, .raw_edges = true },	// PB6: FM voice PTT
};

extern const hal_flash_t rt880_flash;	// boards/rt880/flash_rt880.c (W25Q32 SPI NOR, bit-banged)
#if CONFIG_HOSTLINK
extern const hal_stream_t rt880_host_stream;	// boards/rt880/serial_rt880.c (USART6 host link for CPS)
#endif
// Battery sense: the stock firmware reads it on ADC1 ORDINARY SEQUENCE 1 = channel 7 (PA7) - confirmed by reverse-
// engineering the factory image (adc_ordinary_channel_set(ADC1, 7, 1, ...) feeding DMA buffer[0], then V = raw*3/51+2
// in tenths of a volt). The divider (BATT -> ~300K -> PA7 -> ~47K -> GND) gives Vbatt = Vadc * 347/47; a full 2S pack
// (8.4 V) lands at Vadc ~1.13 V, raw ~1400, matching the stock. (An earlier build used channel 6/PA6, which read an
// unrelated ~2.9 V rail - hence the bogus ~22 V reading.)
// AT32F423 ADC1 is the Artery ADC (base 0x40012000): STS 0x00 (OCCE = bit 1), CTRL2 0x08 (ADCEN bit0, ADCAL bit2,
// ADCALINIT bit3, EOCSFEN bit10, OCTESEL [27:24], OCETE [29:28], OCSWTRG bit30), SPT2 0x10 (cspt7 [23:21]),
// OSQ1 0x2C (OCLEN [24:20]), OSQ3 0x34 (osn1 [4:0]), ODT 0x4C. The ADC1 clock is CRM APB2EN (0x40023844) bit 8, and
// the ADC clock divider is ADCCOM CCTRL (0x40012304) adcdiv [19:16] off HCLK. The 300K source is high-impedance, so
// channel 7 gets the longest sample time. (Bit positions confirmed against the AT32F423 reference manual / firmware
// library - the Artery ADC does NOT share the classic STM32F1/AT32F403 CTRL2/STS layout.)
#define REGW(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define ADC1		0x40012000u
#define CRM_APB2EN	0x40023844u	// ADC1EN = bit 8
#define ADCCOM_CCTRL	0x40012304u	// adcdiv [19:16]
#define BATT_CH		7u		// PA7 (ordinary sequence 1 in the stock firmware)
uint16_t rt880_batt_raw;	// last raw 12-bit ADC reading, surfaced on a diagnostics line for bring-up/calibration

static bool adc_ready;

static void adc_setup(void)
{
	REGW(CRM_APB2EN) |= 1u << 8;			// CRM APB2EN: ADC1 peripheral clock
	REGW(ADCCOM_CCTRL) = (REGW(ADCCOM_CCTRL) & ~(0xFu << 16)) | (0x6u << 16);	// adcdiv = HCLK/8 (~6 MHz)
	// PA7 to analog: GPIOA CFGR (MODER) field = 0b11, no pull.
	REGW(0x40020000u) |= 3u << (BATT_CH * 2);
	REGW(0x4002000Cu) &= ~(3u << (BATT_CH * 2));
	REGW(ADC1 + 0x08u) |= 1u;			// CTRL2: ADCEN
	for (uint32_t i = 0; i < 100000u && !(REGW(ADC1 + 0x00u) & (1u << 6)); i++) {}	// wait STS RDY (bit 6)
	// Calibration: reset the calibration (ADCALINIT, bit 3), then run it (ADCAL, bit 2); each self-clears.
	REGW(ADC1 + 0x08u) |= 1u << 3;
	for (uint32_t i = 0; i < 100000u && (REGW(ADC1 + 0x08u) & (1u << 3)); i++) {}
	REGW(ADC1 + 0x08u) |= 1u << 2;
	for (uint32_t i = 0; i < 100000u && (REGW(ADC1 + 0x08u) & (1u << 2)); i++) {}
	REGW(ADC1 + 0x10u) = (REGW(ADC1 + 0x10u) & ~(7u << 21)) | (7u << 21);	// SPT2: channel 7 longest sample time
	REGW(ADC1 + 0x2Cu) &= ~(0x1Fu << 20);		// OSQ1: ordinary sequence length = 1 conversion (OCLEN = 0)
	REGW(ADC1 + 0x34u) = (REGW(ADC1 + 0x34u) & ~0x1Fu) | BATT_CH;	// OSQ3: first (only) channel = 7
	REGW(ADC1 + 0x08u) |= 1u << 10;			// CTRL2: EOCSFEN - set OCCE at end of each conversion
	// Ordinary trigger = software (OCTESEL = 0x7) with a rising edge (OCETE = 0x1), as the library configures it.
	REGW(ADC1 + 0x08u) = (REGW(ADC1 + 0x08u) & ~((0xFu << 24) | (3u << 28))) | (0x7u << 24) | (0x1u << 28);
	adc_ready = true;
}

static uint16_t adc_read_batt(void)
{
	if (!adc_ready) adc_setup();
	REGW(ADC1 + 0x00u) = ~(1u << 1);		// clear STS OCCE (rc_w0: write 0 to bit 1, 1 elsewhere)
	REGW(ADC1 + 0x08u) |= 1u << 30;			// CTRL2: OCSWTRG - start the ordinary conversion
	for (uint32_t i = 0; i < 1000000u && !(REGW(ADC1 + 0x00u) & (1u << 1)); i++) {}	// poll OCCE (bit 1)
	return (uint16_t)(REGW(ADC1 + 0x4Cu) & 0xFFFu);	// ODT: 12-bit result
}

static uint16_t battery_mv(void)
{
	uint32_t acc = 0;
	for (int i = 0; i < 8; i++) acc += adc_read_batt();	// average 8 samples to settle the jitter
	uint16_t raw = (uint16_t)(acc / 8u);
	rt880_batt_raw = raw;
	// Vadc_mv = raw * 3300 / 4095; Vbatt = Vadc * 347/47 (the on-board divider).
	uint32_t vadc = (uint32_t)raw * 3300u / 4095u;
	return (uint16_t)(vadc * 347u / 47u);
}

// 2S Li-ion pack: ~8.4 V full, ~6.0 V empty, clamped 0..100 (coarse linear gauge).
static uint8_t battery_pct(void)
{
	uint16_t mv = battery_mv();
	if (mv <= 6000u) return 0;
	if (mv >= 8300u) return 100;
	return (uint8_t)((mv - 6000u) * 100u / (8300u - 6000u));
}
static int8_t volume(void) { return -1; }
static bool switch_on(void) { return true; }
// Bi-color status LED D500: green = PA13 (PDA/RX-CLGLED, via R533), red = PA14 (PCK/TX-RLED, via R534); both active
// high (the pin sources through the resistor into the LED to GND). These pins default to SWDIO/SWCLK (the J500 SWD
// programming header) - driving them as GPIO at runtime is fine; SWD still works under reset for flashing. The core's
// radio state machine calls this with LED_GREEN (RX/busy) and LED_RED (TX).
static void led(uint8_t which, bool on)
{
	at32_pin_output(AT32_GPIO_A, which == LED_RED ? 14u : 13u, on);
}
static const hal_power_t power = {
	.init = nothing, .latch = ignore_bool, .battery_mv = battery_mv, .battery_pct = battery_pct,
	.volume = volume, .switch_on = switch_on, .led = led,
};
#if CONFIG_BCAST_FM
// Probe the Si4732 once (it blocks ~150 ms with a power cycle) and cache the line. "Si4732 fwX.Y" means the chip
// answered on I2C - so if there is still no HF audio, the audio path is the suspect, not the wiring. "no I2C" means
// nothing acknowledged at address 0x11: check PC0/PC1/PC5 and the SEN-selected address.
static bool hf_probed;
static char hf_status[24];
static const char *hf_diag_line(void)
{
	if (!hf_probed) {
		uint8_t pn = 0, fj = 0, fn = 0;
		if (si4732_hal_probe(&pn, &fj, &fn)) {
			snprintf(hf_status, sizeof hf_status, "HF Si47%02u fw%u.%u", (unsigned)pn, (unsigned)fj, (unsigned)fn);
		} else {
			snprintf(hf_status, sizeof hf_status, "HF: no I2C PC0/1/5?");
		}
		hf_probed = true;
	}
	return hf_status;
}
// Live state the core last read from the chip: whether it is powered, the frequency it reports, and the RSSI that
// drives the squelch. On a bench with no HF antenna the RSSI stays low and the squelch keeps the audio shut - open
// the monitor to hear it. rssi climbing with signal but still no sound points at the audio path, not the chip.
static bool hf_live_line(char *buf, uint8_t len)
{
	bcast_status_t st;
	if (si4732_hal_last(&st)) {
		snprintf(buf, len, "HF %lukHz R:%u %s", (unsigned long)st.khz, (unsigned)st.rssi, si4732_hal_demod_name());
	} else {
		snprintf(buf, len, "HF idle (tune 0.15-24MHz)");
	}
	return true;
}
#ifdef RT880_HF_ANTPROBE
// Antenna / LNA pin finder. The Si4732 answers I2C but is deaf on HF, so the factory likely routes the HF antenna
// (or powers an HF LNA) through a GPIO we do not drive. This cycles each candidate pin HIGH then LOW for ~2 s and
// shows it on this line. Tune a known strong MW/SW station, watch the HF RSSI on the line above: when it jumps, the
// pin shown here is the antenna / LNA switch. Candidates are the pins with no other assignment.
static const struct { uint32_t port; unsigned pin; const char *name; } ant_cand[] = {
	{ AT32_GPIO_A, 0, "PA0" }, { AT32_GPIO_A, 1, "PA1" }, { AT32_GPIO_A, 2, "PA2" }, { AT32_GPIO_A, 5, "PA5" },
	{ AT32_GPIO_A, 6, "PA6" }, { AT32_GPIO_A, 10, "PA10" }, { AT32_GPIO_B, 6, "PB6" }, { AT32_GPIO_B, 8, "PB8" },
	{ AT32_GPIO_B, 9, "PB9" }, { AT32_GPIO_B, 11, "PB11" }, { AT32_GPIO_C, 3, "PC3" }, { AT32_GPIO_F, 1, "PF1" },
};
#define ANT_CAND_N (sizeof ant_cand / sizeof ant_cand[0])
static uint16_t ant_tick;
static uint8_t ant_idx;
static bool ant_level;
static bool ant_probe_line(char *buf, uint8_t len)
{
	if (++ant_tick >= 40u) {		// ~2 s per state at the diag refresh rate
		ant_tick = 0;
		at32_pin_input(ant_cand[ant_idx].port, ant_cand[ant_idx].pin);	// release the pin we just drove
		if (ant_level) {
			ant_level = false;
			ant_idx = (uint8_t)((ant_idx + 1u) % ANT_CAND_N);
		} else {
			ant_level = true;
		}
	}
	at32_pin_output(ant_cand[ant_idx].port, ant_cand[ant_idx].pin, ant_level);
	snprintf(buf, len, "ANT %s=%u (%u/%u)", ant_cand[ant_idx].name, (unsigned)ant_level,
		(unsigned)(ant_idx + 1u), (unsigned)ANT_CAND_N);
	return true;
}
#define HF_DIAG_LINES 3
#else
#define HF_DIAG_LINES 2
#endif
#else
#define HF_DIAG_LINES 0
#endif
#if CONFIG_MESSENGER
// GOGUFW analog messenger monitor: arm BK4819 FSK receive once, poll each refresh, and show the last decoded message.
// Set both radios to the same FM frequency; a message sent from a GOGUFW UV-K1/K5 shows up here. To test the TX path,
// hold the * and # keys together on this line: the radio sends one canned GGM2 text frame, then returns to listening -
// a GOGUFW radio (or a second RT-880) on the same frequency decodes it. The chord is used so a plain digit never keys up.
#define MSG_TX_CHORD	((1u << 3) | (1u << 11))	// * (bit 3) + # (bit 11), both held
static bool msg_armed_once;
static bool msg_tx_chord_prev;
static bool msg_tx_sent;
static bool msg_diag_line(char *buf, uint8_t len)
{
	if (!msg_armed_once) {
		rt880_msg_rx_arm();
		msg_armed_once = true;
	}
	bool chord = (last_keys & MSG_TX_CHORD) == MSG_TX_CHORD;
	if (chord && !msg_tx_chord_prev) {
		msg_tx_sent = rt880_msg_send("RT880 TEST");	// one burst, then rt880_msg_send re-arms RX
	}
	msg_tx_chord_prev = chord;
	if (chord) {
		snprintf(buf, len, "MSG: TX %s", msg_tx_sent ? "sent" : "fail");
		return true;
	}
	rt880_msg_poll();
	msg_packet_t m;
	if (rt880_msg_last(&m)) {
		snprintf(buf, len, "MSG %s:%.16s", m.from, (const char *)m.payload);
	} else {
		snprintf(buf, len, "MSG: listening");
	}
	return true;
}
#endif
static bool diag(uint8_t line, char *buf, uint8_t len)
{
	switch (line) {
	case 0: snprintf(buf, len, "RX EXPERIMENT / NO TX"); return true;
	case 1: snprintf(buf, len, "Keys: %04lX", (unsigned long)last_keys); return true;
	case 2: snprintf(buf, len, "No storage writes"); return true;
	case 3: snprintf(buf, len, "CPU %lu MHz", (unsigned long)(AT32_CORE_HZ / 1000000u)); return true;
	default: break;
	}
	uint8_t n = 4;
#if CONFIG_MESSENGER
	if (line == n) {
		return msg_diag_line(buf, len);
	}
	n++;
#endif
#if CONFIG_BCAST_FM
	if (line == n) {
		snprintf(buf, len, "%s", hf_diag_line());
		return true;
	}
	n++;
	if (line == n) {
		return hf_live_line(buf, len);
	}
	n++;
#ifdef RT880_HF_ANTPROBE
	if (line == n) {
		return ant_probe_line(buf, len);
	}
	n++;
#endif
#endif
	return rt880_rf_diag((uint8_t)(line - n), buf, len);
}
// Bands. The aeronautical band (108-137 MHz) is RX-only and received in AM:
// transmitting on aviation frequencies is dangerous and illegal, so its tx flag
// stays false and the core refuses TX there. The two wideband FM ranges allow TX
// (licensed operation only). The tx flag is the single gate the core checks.
static const band_t bands[] = {
	{ 108000000, 137000000, false },	// air band: AM RX only, never TX
	{ 136000000, 174000000, true },		// VHF FM: RX + TX
	{ 400000000, 480000000, true },		// UHF FM: RX + TX
#if CONFIG_BCAST_FM
	// HF / AM on the Si4732, received as a second VFO: LW/MW/SW up to 30 MHz, RX only, never TX. This entry is what
	// lets the UI accept an HF frequency at all (board_in_band); the core then routes it to the chip because it falls
	// in the broadcast receiver's range. It must span SI4732_LO_KHZ..SI4732_HI_KHZ.
	{ 150000, 30000000, false },		// HF: 0.15-30 MHz, Si4732, RX only
#endif
};
static const char *const power_names[] = { "LO", "HI" };
const board_t board = {
	.name = "RT-880 LAB",
	.caps = CAP_KEYPAD | (CONFIG_AM_RX ? CAP_AM_RX : 0) | (CONFIG_BCAST_FM ? CAP_BCAST_FM : 0),
	.bands = bands, .band_count = sizeof(bands) / sizeof(bands[0]), .power_count = 2, .power_names = power_names,
	.display = &display, .input = &input, .keymap = keys, .keymap_len = sizeof(keys) / sizeof(keys[0]),
	.radio = &rt880_radio, .power = &power, .flash = &rt880_flash, .diag_line = diag,
#if CONFIG_HOSTLINK
	.host_port = &rt880_host_stream,	// USART6 on the accessory FPC (boards/rt880/serial_rt880.c): CPS / host link
#endif
#if CONFIG_BCAST_FM
	.bcast = &si4732_hal,
#endif
};
