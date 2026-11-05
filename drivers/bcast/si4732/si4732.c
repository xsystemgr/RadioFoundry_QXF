// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Si4732 command set (si4732.h), per Silicon Labs AN332.
#include "si4732.h"
#ifndef CONFIG_SI4732_SSB
#define CONFIG_SI4732_SSB 0
#endif
#if CONFIG_SI4732_SSB
// The SSB patch bytes are third-party (Silicon Labs / V.Afonkin via PU2CLR), personal/non-commercial use only (see
// drivers/bcast/si4732/README.md). They live in si4732_ssb_patch.h beside this file; if that file is absent the build
// still compiles with SSB disabled (AM only).
#  if defined(__has_include) && __has_include("si4732_ssb_patch.h")
#    include "si4732_ssb_patch.h"
#  else
#    warning "si4732_ssb_patch.h not present: building without SSB (AM only). See the driver README."
#    undef CONFIG_SI4732_SSB
#    define CONFIG_SI4732_SSB 0
#  endif
#endif

// Commands
#define CMD_POWER_UP		0x01u
#define CMD_GET_REV		0x10u
#define CMD_SET_PROPERTY	0x12u
#define CMD_GET_INT_STATUS	0x14u
#define CMD_POWER_DOWN		0x11u
#define CMD_FM_TUNE_FREQ	0x20u
#define CMD_FM_SEEK_START	0x21u
#define CMD_FM_TUNE_STATUS	0x22u
#define CMD_FM_RSQ_STATUS	0x23u
#define CMD_AM_TUNE_FREQ	0x40u
#define CMD_AM_SEEK_START	0x41u
#define CMD_AM_TUNE_STATUS	0x42u
#define CMD_AM_RSQ_STATUS	0x43u

// POWER_UP ARG1 bits: XOSCEN selects the 32.768 kHz watch crystal these radios fit; FUNC picks the front end.
#define PU_XOSCEN		0x10u
#define PU_FUNC_FM		0x00u
#define PU_FUNC_AM		0x01u
#define PU_OPMODE_ANALOG	0x05u	// ARG2: analog L/R audio out

// Reply status bit 7 = CTS (clear to send); tune/seek status bit 0 = STCINT (operation complete)
#define STATUS_CTS		0x80u
#define INT_STCINT		0x01u

#define PROP_RX_VOLUME			0x4000u
// AM front-end audio. The chip powers up with soft mute attenuating anything weak (so a SW/HF station with no preamp
// sounds faint or drops out) and a modest AVC ceiling. Opening the soft mute and raising the AVC gain is what makes
// weak HF audible and loud; a 4 kHz channel filter keeps AM broadcast clear.
#define PROP_AM_CHANNEL_FILTER		0x3102u
#define PROP_AM_AVC_MAX_GAIN		0x3103u	// default 0x1543 (~48 dB); 0x7800 is the maximum (~90 dB)
#define PROP_AM_SOFT_MUTE_MAX_ATTEN	0x3302u	// dB of soft-mute attenuation; 0 disables it
#define AM_CHANNEL_FILTER_4K		0x0001u

// SSB (patch) properties and power-up bits
#define PROP_SSB_BFO			0x0100u	// SSB fine-tune: signed Hz offset
#define PROP_SSB_MODE			0x0101u	// SSB demod config: 0x8011 = 2.2 kHz audio BW, AVC off, AFC off (voice)
#define SSB_MODE_VOICE			0x8011u
#define PU_PATCH			0x20u	// POWER_UP ARG1: a patch will be downloaded before normal operation
// SSB tune (AM_TUNE_FREQ ARG1): USBLSB occupies bits 6-7 (USB = 10b, LSB = 01b), FAST = bit 0
#define SSB_ARG1_USB			0x80u
#define SSB_ARG1_LSB			0x40u
#define SSB_ARG1_FAST			0x01u

#define CTS_POLLS		200	// at most 200 x 1 ms waiting for CTS
#define TUNE_POLLS		60	// at most 60 x 5 ms waiting for a tune/seek to settle
#define TUNE_POLL_MS		5

// The FM front end tunes in 10 kHz units, AM in 1 kHz. Anything at or below this is AM (LW/MW/SW); above it is FM.
#define FM_FLOOR_KHZ		30000u

void si4732_attach(si4732_t *d, const si4732_bus_t *bus)
{
	d->bus = bus;
	d->band = SI4732_BAND_AM;
	d->demod = SI4732_DEMOD_AM;
	d->override = SI4732_MODE_AUTO;
	d->bfo = 0;
	d->powered = false;
	d->patched = false;
}

si4732_demod_t si4732_current_demod(const si4732_t *d)
{
	return d->demod;
}

void si4732_set_override(si4732_t *d, uint8_t mode)
{
	d->override = mode > SI4732_MODE_USB ? SI4732_MODE_AUTO : mode;
}

// The demod a frequency wants. The SSB ham voice segments (ITU Region 1) use the patch - LSB below 10 MHz, USB above,
// the standard convention; everything else (MW/SW broadcast) is plain AM. Without the SSB patch built in, always AM.
static si4732_demod_t demod_for(uint32_t khz)
{
#if CONFIG_SI4732_SSB
	static const struct { uint16_t lo, hi; } ssb_seg[] = {
		{ 1840, 2000 }, { 3600, 3800 }, { 7060, 7200 }, { 14100, 14350 },
		{ 18110, 18168 }, { 21150, 21450 }, { 24930, 24990 }, { 28300, 29700 },
	};
	for (unsigned i = 0; i < sizeof ssb_seg / sizeof ssb_seg[0]; i++) {
		if (khz >= ssb_seg[i].lo && khz <= ssb_seg[i].hi) {
			return khz < 10000u ? SI4732_DEMOD_LSB : SI4732_DEMOD_USB;
		}
	}
#endif
	(void)khz;
	return SI4732_DEMOD_AM;
}

static void chip_delay(si4732_t *d, uint32_t ms)
{
	if (d->bus->delay != NULL) {
		d->bus->delay(ms);
	}
}

// Wait until the chip raises CTS, so the next command is accepted. Each poll reads one status byte.
static bool wait_cts(si4732_t *d)
{
	for (int i = 0; i < CTS_POLLS; i++) {
		uint8_t st;
		if (d->bus->resp(d->bus->ctx, &st, 1) && (st & STATUS_CTS)) {
			return true;
		}
		chip_delay(d, 1);
	}
	return false;
}

// Send a command and wait for the chip to report ready. Does not read the command's own reply payload.
static bool command(si4732_t *d, const uint8_t *buf, size_t n)
{
	if (!d->bus->cmd(d->bus->ctx, buf, n)) {
		return false;
	}
	return wait_cts(d);
}

static bool set_property(si4732_t *d, uint16_t prop, uint16_t val)
{
	const uint8_t c[6] = {
		CMD_SET_PROPERTY, 0x00u,
		(uint8_t)(prop >> 8), (uint8_t)prop,
		(uint8_t)(val >> 8), (uint8_t)val,
	};
	return command(d, c, sizeof(c));
}

static bool power_up(si4732_t *d, si4732_band_t band)
{
	const uint8_t c[3] = {
		CMD_POWER_UP,
		(uint8_t)(PU_XOSCEN | (band == SI4732_BAND_FM ? PU_FUNC_FM : PU_FUNC_AM)),
		PU_OPMODE_ANALOG,
	};
	if (!d->bus->cmd(d->bus->ctx, c, sizeof(c))) {
		return false;
	}
	chip_delay(d, 110);		// the oscillator and the selected front end need time before CTS
	if (!wait_cts(d)) {
		return false;
	}
	d->band = band;
	d->powered = true;

	// Make weak HF loud and clear: open the soft mute, raise the AVC ceiling, use a 4 kHz AM filter. Then full chip
	// volume - this radio has no volume knob of its own, so the HAL/core drive the level from here.
	if (band == SI4732_BAND_AM) {
		(void)set_property(d, PROP_AM_SOFT_MUTE_MAX_ATTEN, 0x0000u);
		(void)set_property(d, PROP_AM_AVC_MAX_GAIN, 0x7800u);
		(void)set_property(d, PROP_AM_CHANNEL_FILTER, AM_CHANNEL_FILTER_4K);
	}
	(void)set_property(d, PROP_RX_VOLUME, 63u);
	d->demod = SI4732_DEMOD_AM;
	d->patched = false;		// a normal power-up wipes any patch that was in the chip's RAM
	return true;
}

#if CONFIG_SI4732_SSB
// Bring the chip up in SSB: power up with the patch-enable bit, stream the patch to its RAM (it is volatile, so this
// happens every time SSB is (re)entered), then configure SSB demod and audio. ~16 KB over I2C takes a moment.
static bool ssb_power_up(si4732_t *d)
{
	if (d->powered) {
		si4732_power_down(d);
	}
	const uint8_t c[3] = {
		CMD_POWER_UP, (uint8_t)(PU_PATCH | PU_XOSCEN | PU_FUNC_AM), PU_OPMODE_ANALOG,
	};
	if (!d->bus->cmd(d->bus->ctx, c, sizeof(c))) {
		return false;
	}
	chip_delay(d, 120);
	if (!wait_cts(d)) {
		return false;
	}
	// Stream the patch in 8-byte command lines straight to the chip. The chip raises CTS after each line.
	for (uint16_t off = 0; off + 8u <= SI4732_SSB_PATCH_SIZE; off += 8u) {
		if (!d->bus->cmd(d->bus->ctx, &si4732_ssb_patch[off], 8) || !wait_cts(d)) {
			return false;
		}
	}
	d->band = SI4732_BAND_AM;	// SSB runs on the AM front end (HF)
	d->powered = true;
	d->patched = true;
	// Voice SSB: narrow audio BW, AFC off; weak-signal audio open and loud, as for AM.
	(void)set_property(d, PROP_SSB_MODE, SSB_MODE_VOICE);
	(void)set_property(d, PROP_AM_SOFT_MUTE_MAX_ATTEN, 0x0000u);
	(void)set_property(d, PROP_AM_AVC_MAX_GAIN, 0x7800u);
	(void)set_property(d, PROP_RX_VOLUME, 63u);
	return true;
}
#endif

bool si4732_probe(si4732_t *d, uint8_t *part, uint8_t *fw_major, uint8_t *fw_minor)
{
	if (part != NULL) {
		*part = 0;
	}
	if (fw_major != NULL) {
		*fw_major = 0;
	}
	if (fw_minor != NULL) {
		*fw_minor = 0;
	}
	// The command's own write ACK is what proves a chip is really there: an empty bus reads back as all-ones, so a
	// reply alone (its CTS bit would look set) cannot be trusted. power_up() checks that ACK.
	if (!d->powered && !power_up(d, SI4732_BAND_AM)) {
		return false;
	}
	// GET_REV: reply = status, PN, FW major, FW minor, PATCH_H, PATCH_L, CMP major, CMP minor, CHIPREV
	const uint8_t c[1] = { CMD_GET_REV };
	if (!command(d, c, sizeof(c))) {
		return false;
	}
	uint8_t r[9] = { 0 };
	if (!d->bus->resp(d->bus->ctx, r, sizeof(r))) {
		return false;
	}
	if (part != NULL) {
		*part = r[1];
	}
	if (fw_major != NULL) {
		*fw_major = r[2];
	}
	if (fw_minor != NULL) {
		*fw_minor = r[3];
	}
	return true;
}

bool si4732_power_down(si4732_t *d)
{
	const uint8_t c[1] = { CMD_POWER_DOWN };
	bool ok = command(d, c, sizeof(c));
	d->powered = false;
	return ok;
}

bool si4732_tune(si4732_t *d, uint32_t khz)
{
	// A manual override forces the demod; AUTO picks it from the frequency.
	si4732_demod_t demod;
	switch (d->override) {
	case SI4732_MODE_AM:  demod = SI4732_DEMOD_AM; break;
	case SI4732_MODE_LSB: demod = SI4732_DEMOD_LSB; break;
	case SI4732_MODE_USB: demod = SI4732_DEMOD_USB; break;
	default:              demod = demod_for(khz); break;
	}
#if !CONFIG_SI4732_SSB
	demod = SI4732_DEMOD_AM;	// no patch built in: SSB is unavailable, fall back to AM
#endif
	bool ssb = demod != SI4732_DEMOD_AM;
	si4732_band_t want = (!ssb && khz > FM_FLOOR_KHZ) ? SI4732_BAND_FM : SI4732_BAND_AM;

#if CONFIG_SI4732_SSB
	// SSB needs the patch loaded. Load it when entering SSB from a non-patched state; a plain tune between SSB
	// frequencies, or a sideband change, needs no reload (only the tune's USBLSB bits change).
	if (ssb) {
		if (!d->patched && !ssb_power_up(d)) {
			return false;
		}
	} else
#endif
	{
		// AM/FM: a band change, or leaving a patched (SSB) state, re-powers the chip into the plain front end.
		if (!d->powered || d->band != want || d->patched) {
			if (d->powered) {
				si4732_power_down(d);
			}
			if (!power_up(d, want)) {
				return false;
			}
		}
	}
	d->demod = demod;

	if (want == SI4732_BAND_FM) {
		uint16_t f = (uint16_t)(khz / 10u);	// 10 kHz units
		const uint8_t c[5] = { CMD_FM_TUNE_FREQ, 0x00u, (uint8_t)(f >> 8), (uint8_t)f, 0x00u };
		return command(d, c, sizeof(c));
	}
	// AM / SSB both tune with AM_TUNE_FREQ in 1 kHz units. For SSB, ARG1 carries the sideband (and FAST); ANTCAP 1
	// helps the SW whip above the MW band, 0 (auto) suits LW/MW.
	uint16_t antcap = (khz >= 1800u) ? 1u : 0u;
	uint8_t arg1 = 0x00u;
#if CONFIG_SI4732_SSB
	if (demod == SI4732_DEMOD_USB) {
		arg1 = SSB_ARG1_USB | SSB_ARG1_FAST;
	} else if (demod == SI4732_DEMOD_LSB) {
		arg1 = SSB_ARG1_LSB | SSB_ARG1_FAST;
	}
#endif
	const uint8_t c[6] = {
		CMD_AM_TUNE_FREQ, arg1, (uint8_t)(khz >> 8), (uint8_t)khz,
		(uint8_t)(antcap >> 8), (uint8_t)antcap,
	};
	bool ok = command(d, c, sizeof(c));
#if CONFIG_SI4732_SSB
	if (ok && ssb && d->bfo != 0) {
		(void)set_property(d, PROP_SSB_BFO, (uint16_t)d->bfo);	// restore the fine-tune after the tune
	}
#endif
	return ok;
}

bool si4732_seek(si4732_t *d, bool up)
{
	if (!d->powered) {
		return false;
	}
	// SEEKUP bit 3, WRAP bit 2 (seek wraps round the band)
	uint8_t arg1 = (uint8_t)((up ? 0x08u : 0x00u) | 0x04u);
	const uint8_t cf[2] = { CMD_FM_SEEK_START, arg1 };
	const uint8_t ca[5] = { CMD_AM_SEEK_START, arg1, 0x00u, 0x00u, (uint8_t)((d->band == SI4732_BAND_AM) ? 1u : 0u) };
	if (d->band == SI4732_BAND_FM) {
		return command(d, cf, sizeof(cf));
	}
	return command(d, ca, sizeof(ca));
}

bool si4732_volume(si4732_t *d, uint8_t level)
{
	if (level > 63u) {
		level = 63u;
	}
	return set_property(d, PROP_RX_VOLUME, level);
}

void si4732_set_bfo(si4732_t *d, int16_t hz)
{
	d->bfo = hz;
	if (d->powered && d->demod != SI4732_DEMOD_AM) {
		(void)set_property(d, PROP_SSB_BFO, (uint16_t)hz);
	}
}

bool si4732_status(si4732_t *d, uint32_t *khz, uint8_t *rssi, bool *valid, bool *stereo, bool *tuning)
{
	if (khz != NULL) {
		*khz = 0;
	}
	if (rssi != NULL) {
		*rssi = 0;
	}
	if (valid != NULL) {
		*valid = false;
	}
	if (stereo != NULL) {
		*stereo = false;
	}
	if (tuning != NULL) {
		*tuning = false;
	}
	if (!d->powered) {
		return false;
	}
	bool fm = (d->band == SI4732_BAND_FM);

	// Live signal quality. RSQ_STATUS reports the CURRENT RSSI and SNR every call, independent of the tune-complete
	// (STC) flag - unlike TUNE_STATUS, which only latches the value at tune time. ARG1 = 0 leaves the interrupts
	// alone (no INTACK), so reading it does not disturb anything. Reply: status, RESP1(bit0 VALID), RESP2
	// (FM bit7 STBLEND stereo), RESP3 RSSI, RESP4 SNR.
	const uint8_t rq[2] = { fm ? CMD_FM_RSQ_STATUS : CMD_AM_RSQ_STATUS, 0x00u };
	if (!d->bus->cmd(d->bus->ctx, rq, sizeof(rq))) {
		return false;
	}
	uint8_t q[8] = { 0 };
	if (!d->bus->resp(d->bus->ctx, q, sizeof(q))) {
		return false;
	}
	if (rssi != NULL) {
		*rssi = q[3];
	}
	if (valid != NULL) {
		*valid = (q[1] & 0x01u) != 0;
	}
	if (stereo != NULL) {
		*stereo = fm && (q[2] & 0x80u) != 0;
	}

	// The frequency the chip is on now. TUNE_STATUS with ARG1 = 0 does NOT clear STC, so it is safe to poll: reply
	// is status, flags, freq_h, freq_l, ... (FM in 10 kHz units, AM in 1 kHz).
	if (khz != NULL) {
		const uint8_t ts[2] = { fm ? CMD_FM_TUNE_STATUS : CMD_AM_TUNE_STATUS, 0x00u };
		uint8_t t[8] = { 0 };
		if (d->bus->cmd(d->bus->ctx, ts, sizeof(ts)) && d->bus->resp(d->bus->ctx, t, sizeof(t))) {
			uint16_t f = (uint16_t)(((uint16_t)t[2] << 8) | t[3]);
			*khz = fm ? (uint32_t)f * 10u : (uint32_t)f;
		}
	}
	return true;
}
