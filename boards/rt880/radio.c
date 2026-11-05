// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Experimental VHF/UHF receive using the existing RadioFoundry BK4819 driver.
#include <stddef.h>
#include <stdio.h>
#include "config.h"
#include "board.h"
#include "radio.h"
#include "platform.h"
#include "hal_time.h"
#include "rf/bk4819/bk4819.h"

static bk4819_t receiver, control;
static uint16_t receiver_id, control_id;
static bool ready, tuned, fm = true, started, am_mode, transmitting;
static radio_bw_t tx_bw = RADIO_BW_25K;
static uint32_t frequency;
static bool drive_a, drive_b, audio_requested, audio_open;
static bool bcast_audio_want;	// the Si4732 broadcast receiver is asking for the shared speaker (CONFIG_BCAST_FM)
static bool hf_bk_mode;		// tuned to the 24-30 MHz experiment on the BK4819: force full AF gain (weak front-end)
static uint8_t volume_percent = 50;
static uint16_t af_readback, gain_readback;
static int16_t last_rssi = -127;
static uint8_t last_noise = 255;
static uint16_t enables_readback, power_readback, voice_level;

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
	ready = tuned = audio_requested = audio_open = bcast_audio_want = false;
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
// The speaker amplifier (PF8 gate, PC4 mute) is shared by the channel receiver and the Si4732 broadcast receiver.
// It must stay open while EITHER wants audio: the core drives the two paths in turn (bcast on, then channel off),
// so if each owned the gate directly the one switched off last would mute the other. Each path mutes its own source
// instead (the BK4819 AF here, the Si4732 volume in its HAL) and they share this gate.
static void speaker_gate(void)
{
	bool spk = audio_open || bcast_audio_want;
	at32_pin_write(AT32_GPIO_F, 8, spk);		// speaker gate
	at32_pin_write(AT32_GPIO_C, 4, !spk);		// audio mute (active high)
}
static void audio_apply(void)
{
	bool on = audio_requested && ready && tuned && volume_percent != 0;
	audio_open = on;
	// The channel receiver's own source mute (the BK4819 AF). Factory AM uses AF select 7 (reg 0x47 = 0x6740);
	// FM uses select 1 (0x6140). The shared speaker gate is set afterwards so the broadcast path can keep it open.
	if (ready) bk4819_set_af(&receiver, on ? (am_mode ? 0x6740u : BK4819_AF_FM) : BK4819_AF_MUTE);
	speaker_gate();
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
#if CONFIG_BCAST_FM
// The Si4732 broadcast receiver claims or releases the shared speaker. Its audio source is muted through its own
// volume (hal_bcast), so here it only shares the amplifier gate with the channel receiver above.
void rt880_bcast_speaker(bool on)
{
	bcast_audio_want = on;
	speaker_gate();
}
#endif
static void volume(uint8_t percent)
{
	volume_percent = percent > 100 ? 100 : percent;
	// On the deaf 24-30 MHz BK4819 path the recovered audio is weak, so run the AF stage wide open; elsewhere follow
	// the requested level.
	uint8_t gain = hf_bk_mode ? 15u : (uint8_t)((volume_percent * 15u + 50u) / 100u);
	if (ready) bk4819_set_af_gain(&receiver, gain);
	audio_apply();
}
static uint16_t route(uint32_t hz)
{
#if CONFIG_BCAST_FM
	// Experiment: 24-30 MHz (CB at 27, the 10 m FM segment) on the BK4819 instead of the Si4732. The BK4819 does
	// narrowband FM, which the Si4732 cannot on HF; its PLL reaches down here, though the VHF front-end is deaf this
	// low, so expect only strong, nearby signals. Below 24 MHz stays on the Si4732 (AM/SW), which never reaches route().
	if (hz >= 24000000u && hz < 30000000u) return 0x10;	// HF-high via the low-band front-end path
#endif
	if (hz >= 108000000u && hz < 137000000u) return 0x10; // aeronautical AM band (VHF front-end)
	if (hz >= 136000000u && hz <= 174000000u) return 0x10;
	if (hz >= 400000000u && hz <= 480000000u) return 0x20;
	return 0;
}
static void init(void)
{
	rt880_rf_halt();
	started = false;
	voice_level = 0;
	volume_percent = 50;
	af_readback = gain_readback = 0;
	drive_a = drive_b = false;
	fm = true;
	frequency = 0;
	at32_pin_output(AT32_GPIO_C, 8, false);
	at32_pin_output(AT32_GPIO_C, 6, false);
	at32_pin_output(AT32_GPIO_F, 6, false);
	at32_pin_output(AT32_GPIO_C, 9, false);
	// Factory firmware (RT-880 V1.16) runs its entire main BK4819 setup -- incl.
	// the AF/audio registers -- on the chip at PF6(clk)/PA8(cs)/PC9(data) = bus_b.
	// The audio amp is fed by THAT chip's AF output. Receiving on bus_a produced
	// only faint bleed. Make bus_b the receiver; bus_a becomes the idle control.
	bk4819_attach(&receiver, &bus_b);
	bk4819_attach(&control, &bus_a);
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
#ifdef RT880_PIN_FINDER
	// Pin finder: hold every unassigned, non-keypad pin as input with pull-up so it
	// idles high (1). Press PTT / any button and watch the Diagnostics port lines for
	// a bit that drops 1->0 (active-low) -- that is the pin.
	{
		static const struct { uint32_t port; unsigned pin; } cand[] = {
			{AT32_GPIO_A,0},{AT32_GPIO_A,1},{AT32_GPIO_A,2},{AT32_GPIO_A,5},{AT32_GPIO_A,6},{AT32_GPIO_A,10},
			{AT32_GPIO_B,6},{AT32_GPIO_B,8},{AT32_GPIO_B,9},{AT32_GPIO_B,11},
			{AT32_GPIO_C,3},{AT32_GPIO_D,2},{AT32_GPIO_F,1},
		};
		for (unsigned i = 0; i < sizeof cand / sizeof cand[0]; i++)
			at32_pin_input_pullup(cand[i].port, cand[i].pin);
	}
#endif
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
static bool do_tune(uint32_t hz, radio_bw_t bw, bool am)
{
	uint16_t path = route(hz);
	if (!ready || !fm || !path || (bw != RADIO_BW_12K5 && bw != RADIO_BW_25K)) return false;
	am_mode = am;
	hf_bk_mode = hz >= 24000000u && hz < 30000000u;	// the HF-via-BK4819 experiment path gets full AF gain
	audio(false);
	bk4819_set_state(&receiver, BK4819_STATE_IDLE);
	// Break before make; only these two RX GPIO outputs may ever be asserted.
	bk4819_write(&control, 0x33, 0);
	bk4819_write(&control, BK4819_REG_PA, 0);
	bk4819_write(&control, BK4819_REG_ENABLES, 0);
	// Band-select GPIO (reg 0x33) lives on the active chip; now that is the receiver.
	bk4819_write(&receiver, 0x33, path);
	bk4819_set_bandwidth(&receiver, bw == RADIO_BW_25K ? BK4819_BW_25K : BK4819_BW_12K5);
	bk4819_set_freq(&receiver, hz);
	// AFC tracks the carrier in FM; in AM it must not pull off the (unmodulated) carrier.
	uint16_t afc = bk4819_read(&receiver, BK4819_REG_AFC);
	bk4819_write(&receiver, BK4819_REG_AFC, am ? (uint16_t)(afc | BK4819_AFC_OFF)
						  : (uint16_t)(afc & ~BK4819_AFC_OFF));
	bk4819_write(&receiver, BK4819_REG_PA, 0);
	// Full AF gain on the deaf HF-via-BK path, otherwise the volume-tracked level.
	bk4819_set_af_gain(&receiver, hf_bk_mode ? 15u : (uint8_t)((volume_percent * 15u + 50u) / 100u));
	bk4819_set_state(&receiver, BK4819_STATE_RX);
	frequency = hz;
	tuned = true;
	hal_delay_ms(10); // conservative acquisition, not an ultra-scan profile
	return true;
}
static bool receive(uint32_t hz, radio_bw_t bw, const subtone_t *tone)
{
	if (tone != NULL && tone->type != TONE_NONE) return false; // FM sub-audio not handled here
	return do_tune(hz, bw, false);
}
#if CONFIG_AM_RX
static bool rx_am(uint32_t hz, radio_bw_t bw)
{
	return do_tune(hz, bw, true);
}
#endif
// reg 0x36 PA bias/gain: bit 7 enables the PA, bits 0-6 are the level. The factory
// reads the level from per-band calibration; until that is parsed these are
// deliberately conservative lab levels, well under BK4819_PA_MAX (0xBF). Test into
// a dummy load first.
static uint16_t tx_pa_word(uint8_t power)
{
	return power >= 1 ? (uint16_t)(0x80u | 0x20u) : (uint16_t)(0x80u | 0x08u);
}
static bool transmit(uint32_t hz, radio_bw_t bw, uint8_t power, const subtone_t *tone)
{
	uint16_t path = route(hz);
	// board_in_band(hz, true) is false in the air band (tx flag false) and out of band.
	if (!ready || !fm || !path || !board_in_band(hz, true)
		|| (bw != RADIO_BW_12K5 && bw != RADIO_BW_25K)) return false;
	bk4819_tone_t tt = BK4819_TONE_NONE;
	uint16_t tv = 0;
	if (tone != NULL && tone->type != TONE_NONE) {
		tt = tone->type == TONE_CTCSS ? BK4819_TONE_CTCSS
		   : tone->type == TONE_DCS ? BK4819_TONE_DCS
		   : tone->type == TONE_DCS_INV ? BK4819_TONE_DCS_INV : BK4819_TONE_NONE;
		tv = tone->value;
	}
	// Mute RX audio, idle the chip, then set the band path, as the factory does.
	audio(false);
	bk4819_set_af(&receiver, BK4819_AF_MUTE);
	bk4819_set_state(&receiver, BK4819_STATE_IDLE);
	bk4819_write(&receiver, 0x33, path);
	// Factory TX band-path relay (PF0): low for the higher band (UHF), high for VHF.
	at32_pin_write(AT32_GPIO_F, 0, path != 0x20);
	bk4819_tx_prepare(&receiver, tt, tv);		// deviation and sub-audio
	bk4819_write(&receiver, BK4819_REG_PA, tx_pa_word(power));
	bk4819_tx_tune(&receiver, hz);			// freq, POWER_ON, STATE_TX_MAIN
	frequency = hz;
	tx_bw = bw;
	transmitting = true;
	return true;
}
static void tx_off(void)
{
	if (!transmitting) return;
	bk4819_tx_end(&receiver);			// AF mute, tone off, ENABLES back to RX
	bk4819_write(&receiver, BK4819_REG_PA, 0);	// PA off
	at32_pin_write(AT32_GPIO_F, 0, false);		// relay back to the RX path
	transmitting = false;
	do_tune(frequency, tx_bw, am_mode);		// re-establish RX on the same frequency
	audio_apply();
}
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
#if CONFIG_AM_RX
	.rx_am = rx_am,
#endif
};
bool rt880_rf_diag(uint8_t line, char *buf, uint8_t len)
{
	switch (line) {
	case 0: snprintf(buf, len, "RF A:%04X B:%04X", receiver_id, control_id); break;
	case 1: snprintf(buf, len, "%s %s", ready ? "RX ready" : "ID failed", transmitting ? "/ TX ON" : "/ TX idle"); break;
	case 2: snprintf(buf, len, "Tune %lu Hz", (unsigned long)frequency); break;
	case 3: snprintf(buf, len, "AF:%04X G:%04X", af_readback, gain_readback); break;
	case 4: snprintf(buf, len, "Audio %s V:%u", audio_open ? "OPEN" : "MUTE", volume_percent); break;
	case 5: snprintf(buf, len, "RSSI:%d N:%u", last_rssi, last_noise); break;
	case 6: snprintf(buf, len, "PC4:%u PF8:%u", (unsigned)at32_pin_read(AT32_GPIO_C, 4), (unsigned)at32_pin_read(AT32_GPIO_F, 8)); break;
	case 7: snprintf(buf, len, "30:%04X 37:%04X", enables_readback, power_readback); break;
	case 8: snprintf(buf, len, "Voice:%04X", voice_level); break;
#ifdef RT880_PIN_FINDER
	case 9: snprintf(buf, len, "A:%04X B:%04X", at32_port_inputs(AT32_GPIO_A), at32_port_inputs(AT32_GPIO_B)); break;
	case 10: snprintf(buf, len, "C:%04X D:%04X", at32_port_inputs(AT32_GPIO_C), at32_port_inputs(AT32_GPIO_D)); break;
	case 11: snprintf(buf, len, "F:%04X (press PTT)", at32_port_inputs(AT32_GPIO_F)); break;
#endif
	default: return false;
	}
	return true;
}
