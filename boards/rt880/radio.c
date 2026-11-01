// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Experimental VHF/UHF receive using the existing RadioFoundry BK4819 driver.
#include <stddef.h>
#include <stdio.h>
#include "radio.h"
#include "platform.h"
#include "hal_time.h"
#include "rf/bk4819/bk4819.h"

static bk4819_t receiver, control;
static uint16_t receiver_id, control_id;
static bool ready, tuned, fm = true, started;
static uint32_t frequency;
static bool drive_a, drive_b, audio_requested, audio_open;
static uint8_t volume_percent = 50;
static uint16_t af_readback, gain_readback;
static int16_t last_rssi = -127;
static uint8_t last_noise = 255;
static bool test_requested, test_ran;
static uint16_t enables_readback, power_readback, voice_level, test_af;
static uint8_t test_pads;

static void cs_a(bool high) { at32_pin_write(AT32_GPIO_C, 7, high); }
static void clk_a(bool high) { at32_pin_write(AT32_GPIO_C, 8, high); }
static void data_a(bool high)
{
	if (!drive_a) { at32_pin_output(AT32_GPIO_C, 6, high); drive_a = true; }
	else at32_pin_write(AT32_GPIO_C, 6, high);
}
static void release_a(void) { at32_pin_input_floating(AT32_GPIO_C, 6); drive_a = false; }
static bool read_a(void) { return at32_pin_read(AT32_GPIO_C, 6); }
static void cs_b(bool high) { at32_pin_write(AT32_GPIO_A, 8, high); }
static void clk_b(bool high) { at32_pin_write(AT32_GPIO_F, 6, high); }
static void data_b(bool high)
{
	if (!drive_b) { at32_pin_output(AT32_GPIO_C, 9, high); drive_b = true; }
	else at32_pin_write(AT32_GPIO_C, 9, high);
}
static void release_b(void) { at32_pin_input_floating(AT32_GPIO_C, 9); drive_b = false; }
static bool read_b(void) { return at32_pin_read(AT32_GPIO_C, 9); }
static const bk4819_bus_t bus_a = {
	.cs = cs_a, .clk = clk_a, .data_out = data_a, .data_release = release_a, .data_in = read_a,
	.delay_us = hal_delay_us, .delay_ms = hal_delay_ms,
	.crit_enter = at32_irq_save, .crit_exit = at32_irq_restore,
};
static const bk4819_bus_t bus_b = {
	.cs = cs_b, .clk = clk_b, .data_out = data_b, .data_release = release_b, .data_in = read_b,
	.delay_us = hal_delay_us, .delay_ms = hal_delay_ms,
	.crit_enter = at32_irq_save, .crit_exit = at32_irq_restore,
};

// Bounded raw frames for fault/reset shutdown: no DWT delays, RTOS or locks.
static void halt_frame(const bk4819_bus_t *bus, uint8_t reg)
{
	bus->clk(false);
	bus->cs(false);
	uint32_t word = (uint32_t)reg << 16;
	for (unsigned bit = 0; bit < 24; bit++) {
		bus->clk(false);
		bus->data_out((word & 0x800000u) != 0);
		bus->clk(true);
		word <<= 1;
	}
	bus->clk(false);
	bus->cs(true);
}
void rt880_rf_halt(void)
{
	uint32_t irq = at32_irq_save();
	ready = tuned = audio_requested = audio_open = false;
	at32_pin_output(AT32_GPIO_C, 4, true); // observed audio mute
	at32_pin_output(AT32_GPIO_F, 8, false); // observed speaker gate off
	at32_pin_output(AT32_GPIO_C, 15, false); // never HF TX
	at32_pin_output(AT32_GPIO_F, 0, false); // keep HF relay path inactive
	at32_pin_output(AT32_GPIO_C, 7, true);
	at32_pin_output(AT32_GPIO_A, 8, true);
	if (started) {
		const bk4819_bus_t *buses[] = { &bus_b, &bus_a };
		for (unsigned i = 0; i < 2; i++) {
			halt_frame(buses[i], BK4819_REG_PA);
			halt_frame(buses[i], BK4819_REG_ENABLES);
			halt_frame(buses[i], 0x33);
		}
	}
	at32_irq_restore(irq);
}
static void audio_apply(void)
{
	bool on = audio_requested && ready && tuned && volume_percent != 0;
	audio_open = on;
	if (!on) at32_pin_write(AT32_GPIO_C, 4, true);
	if (ready) bk4819_set_af(&receiver, on ? BK4819_AF_FM : BK4819_AF_MUTE);
	at32_pin_write(AT32_GPIO_F, 8, on);
	if (on) at32_pin_write(AT32_GPIO_C, 4, false);
	if (ready) {
		af_readback = bk4819_read(&receiver, BK4819_REG_AF);
		gain_readback = bk4819_read(&receiver, BK4819_REG_AF_GAIN);
		enables_readback = bk4819_read(&receiver, BK4819_REG_ENABLES);
		power_readback = bk4819_read(&receiver, BK4819_REG_POWER);
	}
}
static void audio(bool on)
{
	audio_requested = on;
	audio_apply();
}
static void volume(uint8_t percent)
{
	volume_percent = percent > 100 ? 100 : percent;
	if (ready) bk4819_set_af_gain(&receiver, (uint8_t)((volume_percent * 15u + 50u) / 100u));
	audio_apply();
}
// Local Tone1 through AF selection 2: receiver/DAC only, never a TX state.
static void startup_audio_test(void)
{
	uint16_t saved70 = bk4819_read(&receiver, BK4819_REG_TONE_CTRL);
	uint16_t saved71 = bk4819_read(&receiver, BK4819_REG_TONE1_FREQ);
	test_requested = false;
	bk4819_write(&receiver, BK4819_REG_TONE_CTRL, 0);
	bk4819_write(&receiver, BK4819_REG_TONE1_FREQ, bk4819_tone_word(1000));
	bk4819_set_af(&receiver, 0x6240); // documented Tone Out for RX
	test_af = bk4819_read(&receiver, BK4819_REG_AF);
	at32_pin_write(AT32_GPIO_F, 8, true);
	hal_delay_ms(20);
	for (unsigned pulse = 0; pulse < 2; pulse++) {
		bk4819_write(&receiver, BK4819_REG_TONE_CTRL, 0xB000);
		at32_pin_write(AT32_GPIO_C, 4, false);
		test_pads = (uint8_t)(at32_pin_read(AT32_GPIO_C, 4) | (at32_pin_read(AT32_GPIO_F, 8) << 1));
		hal_delay_ms(150);
		at32_pin_write(AT32_GPIO_C, 4, true);
		bk4819_write(&receiver, BK4819_REG_TONE_CTRL, 0);
		hal_delay_ms(150);
	}
	bk4819_write(&receiver, BK4819_REG_TONE1_FREQ, saved71);
	bk4819_write(&receiver, BK4819_REG_TONE_CTRL, saved70);
	test_ran = true;
	audio_apply(); // restore the core's request, gain and speaker state unchanged
}
static uint16_t route(uint32_t hz)
{
	if (hz >= 136000000u && hz <= 174000000u) return 0x10;
	if (hz >= 400000000u && hz <= 480000000u) return 0x20;
	return 0;
}
static void init(void)
{
	rt880_rf_halt();
	started = false;
	test_requested = rt880_audio_test_boot_requested();
	test_ran = false;
	test_af = test_pads = voice_level = 0;
	volume_percent = 50;
	af_readback = gain_readback = 0;
	drive_a = drive_b = false;
	fm = true;
	frequency = 0;
	at32_pin_output(AT32_GPIO_C, 8, false);
	at32_pin_output(AT32_GPIO_C, 6, false);
	at32_pin_output(AT32_GPIO_F, 6, false);
	at32_pin_output(AT32_GPIO_C, 9, false);
	bk4819_attach(&receiver, &bus_a);
	bk4819_attach(&control, &bus_b);
	receiver_id = bk4819_read(&receiver, BK4819_REG_ID);
	control_id = bk4819_read(&control, BK4819_REG_ID);
	// A missing or different chip is not permission to run an initialization table.
	if (receiver_id != BK4819_ID || control_id != BK4819_ID) return;
	started = true;
	bk4819_write(&control, BK4819_REG_PA, 0);
	bk4819_write(&control, BK4819_REG_ENABLES, 0);
	bk4819_write(&control, 0x33, 0);
	bk4819_write(&control, BK4819_REG_POWER, BK4819_POWER_ON);
	bk4819_init_rx_only(&receiver);
	bk4819_analog_mode(&receiver);
	bk4819_write(&receiver, BK4819_REG_PA, 0);
	ready = true;
	volume(volume_percent);
}
static void mode(radio_mode_t value)
{
	fm = value == RADIO_MODE_FM;
	if (!fm && ready) {
		audio(false);
		bk4819_set_state(&receiver, BK4819_STATE_IDLE);
		bk4819_write(&control, 0x33, 0);
		tuned = false;
	}
}
static bool receive(uint32_t hz, radio_bw_t bw, const subtone_t *tone)
{
	uint16_t path = route(hz);
	if (!ready || !fm || !path || (bw != RADIO_BW_12K5 && bw != RADIO_BW_25K)
		|| (tone != NULL && tone->type != TONE_NONE)) return false;
	audio(false);
	bk4819_set_state(&receiver, BK4819_STATE_IDLE);
	// Break before make; only these two RX GPIO outputs may ever be asserted.
	bk4819_write(&control, 0x33, 0);
	bk4819_write(&control, BK4819_REG_PA, 0);
	bk4819_write(&control, BK4819_REG_ENABLES, 0);
	bk4819_write(&control, 0x33, path);
	bk4819_set_bandwidth(&receiver, bw == RADIO_BW_25K ? BK4819_BW_25K : BK4819_BW_12K5);
	bk4819_set_freq(&receiver, hz);
	bk4819_write(&receiver, BK4819_REG_PA, 0);
	bk4819_set_state(&receiver, BK4819_STATE_RX);
	frequency = hz;
	tuned = true;
	hal_delay_ms(10); // conservative acquisition, not an ultra-scan profile
	if (test_requested) startup_audio_test();
	return true;
}
static bool transmit(uint32_t hz, radio_bw_t bw, uint8_t power, const subtone_t *tone)
{
	(void)hz; (void)bw; (void)power; (void)tone;
	return false;
}
static void tx_off(void) {} // TX cannot start; do not disturb the active receiver
static void status(radio_status_t *value)
{
	*value = (radio_status_t){ .rssi_dbm = -127, .noise = 255 };
	if (!ready || !tuned) return;
	uint16_t raw = bk4819_read(&receiver, BK4819_REG_RSSI);
	value->rssi_dbm = bk4819_rssi_dbm(raw);
	value->noise = (uint8_t)(bk4819_read(&receiver, BK4819_REG_NOISE) & 0x7Fu);
	value->tone_ok = true;
	last_rssi = value->rssi_dbm;
	last_noise = value->noise;
	voice_level = bk4819_read(&receiver, 0x64);
}
const hal_radio_t rt880_radio = {
	.init = init, .set_mode = mode, .rx = receive, .tx = transmit,
	.tx_off = tx_off, .status = status, .audio = audio, .volume = volume,
};
bool rt880_rf_diag(uint8_t line, char *buf, uint8_t len)
{
	switch (line) {
	case 0: snprintf(buf, len, "RF A:%04X B:%04X", receiver_id, control_id); break;
	case 1: snprintf(buf, len, "RX %s / TX blocked", ready ? "ready" : "ID failed"); break;
	case 2: snprintf(buf, len, "Tune %lu Hz", (unsigned long)frequency); break;
	case 3: snprintf(buf, len, "AF:%04X G:%04X", af_readback, gain_readback); break;
	case 4: snprintf(buf, len, "Audio %s V:%u", audio_open ? "OPEN" : "MUTE", volume_percent); break;
	case 5: snprintf(buf, len, "RSSI:%d N:%u", last_rssi, last_noise); break;
	case 6: snprintf(buf, len, "PC4:%u PF8:%u", (unsigned)at32_pin_read(AT32_GPIO_C, 4), (unsigned)at32_pin_read(AT32_GPIO_F, 8)); break;
	case 7: snprintf(buf, len, "30:%04X 37:%04X", enables_readback, power_readback); break;
	case 8: snprintf(buf, len, "Voice:%04X", voice_level); break;
	case 9: snprintf(buf, len, "Test %s", test_ran ? "sent" : "hold * at boot"); break;
	case 10: snprintf(buf, len, "TestAF:%04X P:%u", test_af, test_pads); break;
	default: return false;
	}
	return true;
}
