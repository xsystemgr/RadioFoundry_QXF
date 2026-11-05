// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The Si4732 as hal_bcast_t (si4732_hal.h)
#include "si4732_hal.h"

static const si4732_hal_cfg_t *wiring;
static si4732_t dev;
static uint32_t last_khz;	// the frequency a power-up returns to, and where a seek leaves the chip
static bcast_status_t last_status;	// the most recent reading, for the board's live diagnostic line
// The level the core last asked for, so audio(on) restores it after a mute. Starts near the top of the 0..15 scale
// (the core overwrites it through volume() before any unmute).
static uint8_t cur_level = BCAST_VOLUME_MAX;

// The chip speaks over I2C: a command is a write, a reply is a plain read (status byte first, no register).
static bool bus_cmd(void *ctx, const uint8_t *buf, size_t n)
{
	(void)ctx;
	return i2c_gpio_write(wiring->bus, wiring->addr7, buf, n);
}

static bool bus_resp(void *ctx, uint8_t *buf, size_t n)
{
	(void)ctx;
	return i2c_gpio_write_read(wiring->bus, wiring->addr7, NULL, 0, buf, n);
}

static void bus_delay(uint32_t ms)
{
	if (wiring->delay_ms != NULL) {
		wiring->delay_ms(ms);
	}
}

const si4732_bus_t si4732_i2c_bus = { bus_cmd, bus_resp, bus_delay, NULL };

void si4732_hal_init(const si4732_hal_cfg_t *cfg)
{
	wiring = cfg;
	si4732_attach(&dev, &si4732_i2c_bus);
	last_khz = cfg->start_khz;
}

bool si4732_hal_probe(uint8_t *part, uint8_t *fw_major, uint8_t *fw_minor)
{
	if (wiring == NULL) {
		return false;
	}
	if (wiring->enable != NULL) {
		wiring->enable(true);
	}
	if (wiring->delay_ms != NULL) {
		wiring->delay_ms(20);
	}
	i2c_gpio_recover(wiring->bus);
	bool ok = si4732_probe(&dev, part, fw_major, fw_minor);
	si4732_power_down(&dev);
	if (wiring->enable != NULL) {
		wiring->enable(false);
	}
	return ok;
}

// hal_bcast volume is 0..15; the Si4732's RX_VOLUME is 0..63. This radio has no volume knob, so the core sends a
// fixed level near the top of the scale (RADIO_BCAST_VOLUME_FIXED, 12); map that to full chip volume so HF is loud,
// while 0 still mutes. The gain of ~1.33 reaches 63 by level 12 and holds there.
static uint8_t scale_volume(uint8_t level)
{
	if (level > BCAST_VOLUME_MAX) {
		level = BCAST_VOLUME_MAX;
	}
	uint16_t v = (uint16_t)level * 84u / 15u;
	return v > 63u ? 63u : (uint8_t)v;
}

static bool hal_power(bool on)
{
	if (on) {
		if (wiring->enable != NULL) {
			wiring->enable(true);
		}
		bus_delay(10);
		// A tuner left holding SDA by a reset mid-transfer would never answer
		i2c_gpio_recover(wiring->bus);
		// Power up on the last frequency, muted. si4732_tune() picks the AM/FM front end and powers the chip.
		if (si4732_tune(&dev, last_khz)) {
			(void)si4732_volume(&dev, 0);
			return true;
		}
		if (wiring->enable != NULL) {
			wiring->enable(false);
		}
		return false;
	}
	bool ok = si4732_power_down(&dev);
	if (wiring->audio_path != NULL) {
		wiring->audio_path(false);
	}
	if (wiring->enable != NULL) {
		wiring->enable(false);
	}
	return ok;
}

static bool hal_tune(uint32_t khz)
{
	if (!si4732_tune(&dev, khz)) {
		return false;
	}
	last_khz = khz;
	return true;
}

static bool hal_seek(bool up)
{
	return si4732_seek(&dev, up);
}

static void hal_volume(uint8_t level)
{
	cur_level = level;
	(void)si4732_volume(&dev, scale_volume(level));
}

static bool hal_status(bcast_status_t *st)
{
	uint32_t khz;
	uint8_t rssi;
	bool valid, stereo, tuning;
	if (!dev.powered || !si4732_status(&dev, &khz, &rssi, &valid, &stereo, &tuning)) {
		return false;
	}
	st->khz = khz;
	st->tuning = tuning;
	st->seek_failed = false;	// the chip reports band-limit through its own flags; not surfaced here
	st->stereo = stereo;
	st->station = valid;
	st->rssi = rssi;
	switch (si4732_current_demod(&dev)) {
	case SI4732_DEMOD_LSB: st->demod = BCAST_DEMOD_LSB; break;
	case SI4732_DEMOD_USB: st->demod = BCAST_DEMOD_USB; break;
	default: st->demod = BCAST_DEMOD_AM; break;
	}
	if (!tuning && khz != 0) {
		last_khz = khz;		// a seek ends on a new frequency; power-up comes back to it
	}
	last_status = *st;
	return true;
}

bool si4732_hal_last(bcast_status_t *st)
{
	*st = last_status;
	return dev.powered;
}

const char *si4732_hal_demod_name(void)
{
	switch (si4732_current_demod(&dev)) {
	case SI4732_DEMOD_LSB: return "LSB";
	case SI4732_DEMOD_USB: return "USB";
	default: return "AM";
	}
}

static void hal_set_demod(uint8_t mode)
{
	si4732_set_override(&dev, mode);
	if (dev.powered) {
		(void)si4732_tune(&dev, last_khz);	// re-tune so the new demod takes effect now
	}
}

static void hal_set_bfo(int16_t hz)
{
	si4732_set_bfo(&dev, hz);
}

static void hal_audio(bool on)
{
	if (!dev.powered) {
		on = false;
	}
	// Mute by taking the chip's volume to 0; unmute restores the level the core last asked for
	(void)si4732_volume(&dev, on ? scale_volume(cur_level) : 0u);
	if (wiring->audio_path != NULL) {
		wiring->audio_path(on);
	}
}

const hal_bcast_t si4732_hal = {
	.lo_khz = SI4732_LO_KHZ,
	.hi_khz = SI4732_HI_KHZ,
	.grid_khz = SI4732_GRID_KHZ,
	.power = hal_power,
	.tune = hal_tune,
	.seek = hal_seek,
	.volume = hal_volume,
	.status = hal_status,
	.audio = hal_audio,
	.set_demod = hal_set_demod,
	.set_bfo = hal_set_bfo,
};
