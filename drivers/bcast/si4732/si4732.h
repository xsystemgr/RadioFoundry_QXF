// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Si4732 broadcast/HF receiver: FM broadcast (64-108 MHz) and AM (LW/MW/SW, 153 kHz - 30 MHz) with analog audio out.
// Silicon Labs AN332 command set. The chip is command-driven, not register random-access: a command is CMD + args,
// and every reply begins with a status byte whose bit 7 (CTS) says the chip is ready for the next command. Switching
// between the FM and AM front ends needs a POWER_DOWN / POWER_UP, so the band follows the tuned frequency here.
//
// SSB (the proprietary patch) is out of scope for this driver; it demodulates AM envelope only. The board supplies the
// command/response transport (over I2C on these radios) through si4732_bus_t; one tuner per radio, so the HAL keeps the
// state static. All calls come from one task.
#ifndef SI4732_H
#define SI4732_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// The transport the board wires up (I2C on the RT-880). cmd() writes n command bytes; resp() reads n reply bytes,
// status byte first. Both return false if the chip did not acknowledge. delay() may sleep.
typedef struct {
	bool (*cmd)(void *ctx, const uint8_t *buf, size_t n);
	bool (*resp)(void *ctx, uint8_t *buf, size_t n);
	void (*delay)(uint32_t ms);
	void *ctx;
} si4732_bus_t;

typedef enum {
	SI4732_BAND_FM = 0,	// POWER_UP FUNC 0: FM broadcast, frequency in 10 kHz units
	SI4732_BAND_AM = 1,	// POWER_UP FUNC 1: LW/MW/SW, frequency in 1 kHz units (AM envelope)
} si4732_band_t;

// How HF is demodulated. AM is the plain envelope detector; LSB/USB need the SSB patch (CONFIG_SI4732_SSB) streamed to
// the chip at power-up. The driver picks the demod from the frequency (ham voice segments -> SSB, else AM).
typedef enum {
	SI4732_DEMOD_AM = 0,
	SI4732_DEMOD_LSB = 1,
	SI4732_DEMOD_USB = 2,
} si4732_demod_t;

// Manual demod choice. AUTO lets the driver pick by frequency (ham segments -> SSB, else AM); the others force it.
typedef enum {
	SI4732_MODE_AUTO = 0,
	SI4732_MODE_AM = 1,
	SI4732_MODE_LSB = 2,
	SI4732_MODE_USB = 3,
} si4732_mode_t;

typedef struct {
	const si4732_bus_t *bus;
	si4732_band_t band;	// the front end that is powered up now
	si4732_demod_t demod;	// how it is demodulating now (AM unless the SSB patch is loaded)
	uint8_t override;	// si4732_mode_t: AUTO, or a forced demod
	int16_t bfo;		// SSB fine-tune offset in Hz (BFO); 0 at band edges
	bool powered;
	bool patched;		// the SSB patch is currently loaded in the chip's RAM
} si4732_t;

// Bind the chip to its transport. The chip is left powered down; si4732_tune() powers up the right front end.
void si4732_attach(si4732_t *d, const si4732_bus_t *bus);

// Power the chip down (FM or AM front end off). True when the chip acknowledged.
bool si4732_power_down(si4732_t *d);

// Probe for the chip: power up the AM front end and read GET_REV. Fills the part number (Si4732 reports 32) and the
// firmware major/minor when they are wanted. False when the chip did not acknowledge on the bus - the wiring, the
// address or the power line is wrong. Leaves the chip powered up on success (the HAL's power()/tune() take it from there).
bool si4732_probe(si4732_t *d, uint8_t *part, uint8_t *fw_major, uint8_t *fw_minor);

// Tune to khz. Picks the AM or FM front end from the frequency and re-powers the chip if the band must change.
// Starts the tune; si4732_status() reports when it has settled. False: the chip did not answer.
bool si4732_tune(si4732_t *d, uint32_t khz);

// Seek up or down from the current frequency; stops at a station or after one pass of the band.
bool si4732_seek(si4732_t *d, bool up);

// The demod in use now, for a diagnostic line ("AM" / "LSB" / "USB").
si4732_demod_t si4732_current_demod(const si4732_t *d);

// Force the demod (si4732_mode_t), or AUTO to pick by frequency. Takes effect on the next tune.
void si4732_set_override(si4732_t *d, uint8_t mode);

// SSB fine-tune: set the BFO offset in Hz (applied immediately when in SSB). Stored for the next SSB tune otherwise.
void si4732_set_bfo(si4732_t *d, int16_t hz);

// Receiver volume, 0..63 (the chip's RX_VOLUME range). True when the chip acknowledged.
bool si4732_volume(si4732_t *d, uint8_t level);

// The current tune state. khz = frequency now, rssi in dBuV, valid = the chip judges this a station,
// stereo = FM stereo pilot (always false on AM), tuning = a tune or seek is still running. Any pointer may be NULL.
bool si4732_status(si4732_t *d, uint32_t *khz, uint8_t *rssi, bool *valid, bool *stereo, bool *tuning);

#endif
