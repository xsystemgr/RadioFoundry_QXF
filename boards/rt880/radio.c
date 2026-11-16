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
#if CONFIG_MESSENGER
#include "rf/bk4819/bk4819_fsk.h"
#include "messenger/msg_packet.h"
#include "messenger/msg_fsk.h"
#include "messenger/msg_store.h"
#include "messenger/msg_radio.h"
#endif

static bk4819_t receiver, control;
static uint16_t receiver_id, control_id;
static bool ready, tuned, fm = true, started, am_mode, transmitting;
// Chip routing for TX. The factory (V1.16) splits the two BK4819: bus_a (write fn 0x801e6d4, PC7/PC8/PC6) is the
// TRANSMIT chip - it carries the microphone ADC, the deviation (REG 0x40), the PA (REG 0x36) and the T/R switch -
// while bus_b (write fn 0x801e660, PA8/PF6/PC9) is the RECEIVE chip whose AF feeds the speaker amplifier. Voice must
// therefore be modulated on bus_a (= `control`), where the mic is physically wired; keying a carrier on bus_b gives
// RF but no audio, which is exactly why the earlier all-on-bus_b build transmitted FSK and a bare carrier yet never
// passed the microphone. The custom analog messenger FSK is DSP-generated (no mic) and already works on bus_b, so it
// stays there; only mic-modulated voice moves to bus_a.
static bool tx_on_receiver;	// this TX uses bus_b (messenger FSK); default (false) = bus_a voice, the factory TX chip
static bk4819_t *tx_active;	// the chip currently keyed, so tx_off() ends the right one
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
static uint16_t voice_level_a, voice_level_b;	// REG 0x64 (mic/voice amplitude) on bus_a / bus_b, for the mic probe
static uint16_t tx_mic_peak;			// peak REG 0x64 on the TX chip seen during the current/last voice TX
static uint16_t tx_r30, tx_r40, tx_r50, tx_r70;	// live readback of the TX chip's key modulation registers during voice TX

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

// External audio codec on its own bit-banged bus: PA1 = clock, PA0 = data-out, PA2 = data-in, PF1 = chip select.
// The factory configures the microphone / TX-audio routing through THIS chip, not the BK4819 - which is why no BK4819
// register ever made voice modulate (RX speaker audio works without the codec, so it is the mic path, not the output).
// This is a faithful copy of the factory power-on sequence (reverse-engineered from the register/GPIO traffic only).
static void codec_clk(bool h) { at32_pin_write(AT32_GPIO_A, 1, h); }
static void codec_dat(bool h) { at32_pin_write(AT32_GPIO_A, 0, h); }
static void codec_cs(bool h)  { at32_pin_write(AT32_GPIO_F, 1, h); }
static void codec_byte(uint8_t b)
{
	for (int i = 7; i >= 0; i--) {		// MSB first, data sampled on the rising clock edge (as the stock bit-bang does)
		codec_clk(false);
		codec_dat((b >> i) & 1u);
		hal_delay_us(2);
		codec_clk(true);
		hal_delay_us(2);
	}
}
static void codec_cmd(const uint8_t *d, unsigned n)
{
	codec_cs(false);			// CS low (assert)
	hal_delay_us(5);
	for (unsigned i = 0; i < n; i++) codec_byte(d[i]);
	hal_delay_us(5);
	codec_cs(true);				// CS high (deassert)
}
static void codec_init(void)
{
	at32_pin_output(AT32_GPIO_A, 1, false);		// clock idle low
	at32_pin_output(AT32_GPIO_A, 0, false);		// data
	at32_pin_input_floating(AT32_GPIO_A, 2);	// data-in (read-back, unused here)
	at32_pin_output(AT32_GPIO_F, 1, true);		// CS idle high
	hal_delay_ms(100);
	{ static const uint8_t c[] = { 0x01 };             codec_cmd(c, sizeof c); }
	hal_delay_ms(50);
	{ static const uint8_t c[] = { 0xE0, 0x11, 0x80 }; codec_cmd(c, sizeof c); }
	hal_delay_ms(25);
	{ static const uint8_t c[] = { 0xE0, 0x11, 0x40 }; codec_cmd(c, sizeof c); }
	hal_delay_ms(5);
	{ static const uint8_t c[] = { 0xE1, 0x02, 0x1C }; codec_cmd(c, sizeof c); }
	hal_delay_ms(10);
	{ static const uint8_t c[] = { 0xE2, 0xE2, 0x3E }; codec_cmd(c, sizeof c); }
	hal_delay_ms(100);
}

// The microphone does NOT reach the BK4819 through its internal mic ADC. The schematic (RT880 V2) shows MIC1 going
// through an analog mux (U501) and the ANC mic amp to the MOD481B pin - the external modulation input of chip B
// (= bus_b). In RX the mux routes MIC1 to chip A for monitoring (which is why the probe saw the mic on bus_a); only
// in TX must it be switched so MIC1 reaches the transmitting chip B. Two MCU lines control it: RXTS-SW = PA7 (U502
// pin 23) and ANC-POWER-EN = PC3 (U502 pin 11, and the one the factory firmware drives). Never touching these is the
// real reason voice never modulated - the transmitting chip's mic input was never connected.
// Microphone routing (confirmed from the schematic + the no-RF mic-sense test): the internal mic (IMIC, selected by
// the PB8 pull-up - never driven) reaches MICI, and the analog mux U501 sends MICI -> MOD4819A -> chip A's MICP pin
// (= our bus_b) when SW2 (ANC-POWER-EN = PC3) is LOW. SW1 (RXTS-SW = PA7) low keeps AF-out on the speaker path. So
// both lines idle LOW and the microphone is already routed to the transmit chip (bus_b); no toggling is needed, the
// lines are just held low. The mic-sense build confirmed bus_b's REG 0x64 tracks speech in this state.
static void mic_path_init(void)
{
	// Proven-good routing from the no-RF mic-sense build: PC3 (ANC-POWER-EN) low routes MIC1 to bus_b, where REG 0x64
	// tracked speech. PA7 low keeps AF-out on the speaker path; PB8 left to its hardware pull-up (internal mic).
	at32_pin_output(AT32_GPIO_A, 7, false);
	at32_pin_output(AT32_GPIO_C, 3, false);
	at32_pin_input_floating(AT32_GPIO_B, 8);
}
static void mic_path_tx(bool on)
{
	(void)on;
	at32_pin_write(AT32_GPIO_A, 7, false);
	at32_pin_write(AT32_GPIO_C, 3, false);
}

// The complete AF / audio-DSP register state captured from the working nicFW880 firmware (reference_audio_path capture).
// Our shared init_main + analog_mode leave a different audio base (notably 0x47 muted and 0x24/0x3D/0x4A/0x54/0x55/0x75
// unset), which left BOTH the RX speaker and the TX mic silent. Programming the exact nicFW base makes the DSP the mic
// flows through correct. Applied at init (so RX uses it) and re-applied after a transmit (so RX is restored). Registers
// that the per-tune / per-bandwidth path owns (0x37, 0x43) are deliberately NOT set here to avoid fighting it.
static void nicfw_af_base(bk4819_t *dev)
{
	bk4819_write(dev, 0x24, 0x8C7E);
	bk4819_write(dev, 0x28, 0x2B38);
	bk4819_write(dev, 0x29, 0x2B24);
	bk4819_write(dev, 0x2B, 0x0000);
	bk4819_write(dev, 0x3D, 0x2AAB);
	bk4819_write(dev, 0x3F, 0x0800);
	bk4819_write(dev, 0x47, 0x6140);	// AF-select = FM (un-muted) - the reference's RX value, not analog_mode's MUTE
	bk4819_write(dev, 0x4A, 0x5450);	// 0x48 (AF DAC gain) is owned by volume() - do not pin it here
	bk4819_write(dev, 0x51, 0x9100);
	bk4819_write(dev, 0x54, 0x9009);
	bk4819_write(dev, 0x55, 0x31A9);
	bk4819_write(dev, 0x73, 0x3800);
	bk4819_write(dev, 0x75, 0xE102);
}

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
	at32_pin_output(AT32_GPIO_C, 15, false); // RF front-end select (stock: low for VHF/UHF)
	at32_pin_output(AT32_GPIO_B, 11, false); // RF front-end select (stock rf_path_set: low for VHF/UHF)
	at32_pin_output(AT32_GPIO_F, 0, false); // band relay (stock: low for the 2 m / 70 cm paths)
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
	codec_init();	// configure the external audio codec (the microphone / TX-audio routing the factory sets up here)
	mic_path_init();	// RXTS-SW (PA7) and ANC-POWER-EN (PC3) as outputs, idle in the RX routing
	// bus_a is the factory's transmit chip, so it carries the microphone. Give it the same static setup as the
	// receiver (REG 0x7D mic-gain = 0xE34A, REG 0x37 regulators, REG 0x47 AF, bandwidth) WITHOUT asserting its RF
	// front-end GPIOs (REG 0x33) at startup - exactly what init_rx_only does. The earlier build left this chip at bare
	// POWER_ON with none of the audio registers, so even keying it could not have modulated. The TX-DSP / mic-ADC
	// enables and the T/R path are raised only at key time in transmit(); here it is parked idle with its PA off.
	// bus_a is the transmit + microphone chip, so it gets the FULL main init - exactly as the working BK4819 board
	// db25d does for its transmit chip (bk4819_init_main + bk4819_analog_mode). init_main runs the REG 0x30 = 0x0302
	// step that enables the TX DSP, which is the block that mixes the live mic ADC into the modulator. The earlier
	// init_rx_only SKIPS that step (it drops any REG 0x30 value with the TX-DSP bits), so the mic ADC stayed alive
	// (VOX/REG 0x64 responded) but never reached the modulator - a 1 kHz tone from the generator transmitted fine
	// while the microphone was silent. analog_mode then selects the live-mic TX audio source (REG 0x50 = 0x3B18).
	bk4819_init_main(&control);
	bk4819_analog_mode(&control);
	bk4819_write(&control, BK4819_REG_PA, 0);
	bk4819_set_state(&control, BK4819_STATE_IDLE);
	// bus_b is BOTH the receiver AND, per the schematic, the voice TRANSMIT chip (schematic chip A: MICI -> MOD4819A ->
	// its MICP pin when PC3/SW2 is low). So it must get the FULL main init too - init_main runs the REG 0x30 = 0x0302
	// TX-DSP step that init_rx_only skips; without it the externally-routed microphone reaches the chip's MICP pin but
	// the TX DSP that modulates it was never enabled, so voice stayed silent even with the mux routed correctly.
	bk4819_init_main(&receiver);
	bk4819_analog_mode(&receiver);
	nicfw_af_base(&receiver);		// the working nicFW audio-DSP base, so RX and TX share the correct AF path
	bk4819_write(&receiver, 0x7D, 0xE952);	// mic sensitivity (nicFW)
	bk4819_write(&receiver, BK4819_REG_PA, 0);
	ready = true;
	volume(volume_percent);
#ifdef RT880_MIC_PROBE
	// Mic probe (bench diagnostic, NO RF): run the microphone ADC + TX-DSP on BOTH chips with the PA off, the T/R bit
	// clear and the antenna not routed, so REG 0x64 (voice amplitude) tracks the mic input without anything radiating.
	// Open Diagnostics, speak into the mic, and watch "mA:xxxx mB:xxxx" (line 8): the number that jumps is the chip the
	// microphone is wired to. If NEITHER moves, the mic is not on a BK4819 MIC pin at all (e.g. the MCU ADC path).
	{
		const uint32_t hz = 145500000u;
		bk4819_t *probe[] = { &control, &receiver };
		for (unsigned i = 0; i < 2; i++) {
			bk4819_write(probe[i], 0x33, 0);		// no T/R, no antenna - nothing can radiate
			bk4819_write(probe[i], BK4819_REG_PA, 0);	// PA off
			bk4819_set_freq(probe[i], hz);			// an LO so the TX DSP / mic ADC run
			bk4819_write(probe[i], 0x40, 0x352A);		// plain deviation
			bk4819_write(probe[i], 0x70, 0x0000);		// no tone generator
			bk4819_set_state(probe[i], BK4819_STATE_TX);	// mic ADC + TX DSP on (carrier-only stage, PA still 0)
		}
		tuned = true;	// so status() keeps sampling REG 0x64 on both chips
		audio(true);	// open the speaker in case the mic self-monitors
	}
#endif
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
#if CONFIG_MESSENGER
	// Keep the analog messenger listening on the current FM channel so a message pops up anywhere (GOGUFW "full watch").
	// Re-arm only when the frequency actually changes, so a frame in progress is never reset by a routine re-tune.
	static uint32_t msg_armed_hz;
	if (!am && !hf_bk_mode) {
		if (frequency != msg_armed_hz) {
			rt880_msg_rx_arm();
			msg_armed_hz = frequency;
		}
	} else {
		msg_armed_hz = 0;	// left FM (AM / HF experiment): the next FM tune re-arms
	}
#endif
	return true;
}
// Our subtone_t.type and bk4819_tone_t share the same order (NONE/CTCSS/DCS/DCS_INV), but map explicitly rather than
// cast, so a future reorder can't break it silently.
static bk4819_tone_t bk_tone(const subtone_t *t)
{
	if (t == NULL) return BK4819_TONE_NONE;
	return t->type == TONE_CTCSS ? BK4819_TONE_CTCSS
	     : t->type == TONE_DCS ? BK4819_TONE_DCS
	     : t->type == TONE_DCS_INV ? BK4819_TONE_DCS_INV : BK4819_TONE_NONE;
}

static subtone_t rx_subtone;	// the tuned channel's CTCSS/DCS, so status() can gate audio on the tone (tone squelch)

static bool receive(uint32_t hz, radio_bw_t bw, const subtone_t *tone)
{
	rx_subtone = tone != NULL ? *tone : (subtone_t){ TONE_NONE, 0 };
	if (!do_tune(hz, bw, false))
		return false;
	// FM sub-audio RX: set up the BK4819 CTCSS/DCS detector (value = Hz x 10 for CTCSS, the octal code for DCS, as
	// subtone_t carries them). status() reads the "tone found" bit and reports tone_ok, and the core mutes the speaker
	// until the matching tone is heard. A plain (no-tone) channel clears the detector and always opens.
	bk4819_set_rx_tone(&receiver, bk_tone(tone), tone != NULL ? tone->value : 0);
	return true;
}
#if CONFIG_AM_RX
static bool rx_am(uint32_t hz, radio_bw_t bw)
{
	return do_tune(hz, bw, true);
}
#endif
// BK4819 REG 0x36 is 16-bit: the HIGH byte is the PA bias - the current into the final that actually sets the output
// power - and the LOW byte is the enable bit (7) plus the gain select. The stock radio keys TX with (bias << 8) | 0xFF
// and takes the bias from per-band calibration. An earlier version wrote only the low byte, leaving the bias at 0, so
// the PA was "on" but radiated nothing (confirmed on hardware: TX stored to Sent yet no carrier). These are deliberately
// conservative lab bias levels, well under full scale (0xFF high byte). Transmit into a proper antenna or dummy load.
// The PA bias (REG 0x36 high byte). The stock forms it as (powerval & 0x3F) << 2, with powerval ~0x29-0x37 at full
// power (~4-5 W). The board advertises two levels ("LO"/"HI", power index 0/1 from the channel) and this maps each to
// a bias byte. Both are tunable at build time (RT880_PA_BIAS_LO / RT880_PA_BIAS_HI, or RT880_PA_BIAS to force a single
// level for a bench test). HI defaults near the stock's full-scale bias; trim it to the measured output. The exact
// watts depend on the per-band calibration the stock keeps in flash, which we do not read - so measure into a dummy
// load / antenna and adjust. Never key without a proper 50 ohm load: an unterminated PA at HI bias can be damaged.
#ifdef RT880_PA_BIAS
#define RT880_PA_BIAS_LO (RT880_PA_BIAS)
#define RT880_PA_BIAS_HI (RT880_PA_BIAS)
#endif
#ifndef RT880_PA_BIAS_LO
#define RT880_PA_BIAS_LO 0x30u		// low power (~hundreds of mW, as set for the bench)
#endif
#ifndef RT880_PA_BIAS_HI
#define RT880_PA_BIAS_HI 0xC0u		// high power - strong, near the stock full-scale bias; trim to measured output
#endif
static uint16_t tx_pa_word(uint8_t power)
{
	// power is the board power-level index (0 = LO, 1 = HI); anything >=1 selects HI.
	uint16_t bias = power >= 1u ? (uint16_t)RT880_PA_BIAS_HI : (uint16_t)RT880_PA_BIAS_LO;
	return (uint16_t)((bias << 8) | 0xFFu);		// bias in the high byte, enable + gain in the low
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
	// BOTH voice and messenger FSK transmit on bus_b = `receiver` = chip "B" in the schematic - the chip whose MOD481B
	// pin is the microphone's external modulation input, and the chip that always keyed the carrier and the FSK fine.
	// bus_a = `control` = chip "A" is the RX-side mic monitor, never the transmitter. The old "voice on bus_a" scheme
	// was wrong: the mic was never on bus_a's modulator, it is injected on bus_b's MOD481B pin through the analog mux.
	bool voice_tx = !tx_on_receiver;
	bk4819_t *tx = &receiver;
	bool uhf = hz >= 400000000u;
	audio(false);
	bk4819_set_af(&receiver, BK4819_AF_MUTE);
	bk4819_set_state(&control, BK4819_STATE_IDLE);		// park bus_a (chip A) - not the transmitter
	bk4819_write(&control, 0x33, 0);
	bk4819_write(&control, BK4819_REG_PA, 0);
	// Stock (V1.16) TX antenna / front-end GPIO state: PC15 / PB11 low, band relay PF0 low; BK4819 REG 0x33 selects the
	// front-end filter (VHF bit 3 = 0x08, UHF bit 2 = 0x04) OR'd with the antenna T/R bit (0x10) while transmitting.
	at32_pin_write(AT32_GPIO_C, 15, false);
	at32_pin_write(AT32_GPIO_B, 11, false);
	at32_pin_write(AT32_GPIO_F, 0, false);
	bk4819_set_bandwidth(tx, bw == RADIO_BW_25K ? BK4819_BW_25K : BK4819_BW_12K5);
	bk4819_write(tx, 0x33, (uint16_t)((uhf ? 0x04u : 0x08u) | 0x10u));
	bk4819_write(tx, 0x70, 0x0000);			// no tone generator - the external mic audio modulates the carrier
	bk4819_tx_prepare(tx, tt, tv);			// deviation and sub-audio
	if (voice_tx) {
		// Route the external microphone to bus_b (chip A) and key it. The mux idles with MIC1 already on bus_b's MICP
		// (PC3/PA7 held low - see mic_path), confirmed by the no-RF mic-sense build.
		//
		// The decisive correction comes from the stock RT-880 firmware disassembly: the stock BK4819 driver NEVER writes
		// REG 0x50 (TX audio) - it stays at its 0x0000 power-on reset throughout its working voice transmit. Our shared
		// driver's analog_mode() forces 0x50 = 0x3B18 (a value reverse-engineered from a different board family), and
		// that value gates the externally-injected microphone out of the modulator: the mic ADC still digitises it (REG
		// 0x64 moved), but it never reaches the carrier. The internal tone generator is a separate source, so the tone
		// still modulated. So: write 0x50 = 0x0000 to match the stock, use the stock's voice-TX state 0xC1FE (REG 0x30),
		// and the stock's mic gain 0x7D = 0xE950. Deviation (0x40) is already set by bk4819_tx_prepare() above.
		mic_path_tx(true);
		tx_mic_peak = 0;
		// FIRST, tear down the messenger's armed FSK modem. The analog messenger keeps FSK RX armed to listen, which
		// leaves REG 0x58 bit0 (FSK enable) = 1; an armed FSK modem takes over the TX-DSP modulator input, so the live
		// mic never reaches the carrier (REG 0x64 still rises - it is measured before the DSP; the tone generator sums
		// in after the mic chain, so it still modulated). This is the one group of registers WE write that the stock /
		// nicFW never touch, so the value-diff missed it. Disable it before keying; tx_off() re-arms FSK RX afterwards.
		bk4819_write(tx, 0x58, 0x0000);			// clear FSK enable: the modulator takes the mic ADC, not the modem
		bk4819_write(tx, 0x59, 0x0000);			// FSK FIFO/RX/TX enables off
		// Replicate the nicFW880 voice-TX register sequence (a complete, working firmware for this exact radio, from
		// its disassembly). Its keying writes, for a no-tone FM voice key: 0x28 = 0x2B38 (compander - we had 0x6B00),
		// 0x2A = 0x7FFF (we had 0x4F18), 0x47 = 0x6140 (AF-select FM), 0x50 = 0x3B20 (live source - we had 0x3B18),
		// 0x7D = 0xE952 (mic gain), 0x40 = deviation with the 0x3000 form, and 0x30 = 0xC1FE. These RX-shared
		// registers are restored in tx_off() so receive keeps working.
		nicfw_af_base(tx);				// ensure the full working audio-DSP base is in place for TX
		bk4819_write(tx, 0x31, 0x0000);			// CTCSS/sub-audio enable off (plain voice)
		bk4819_write(tx, 0x19, 0x1041);			// mic AGC (UV-K5 value) - lifts quiet speech
		bk4819_write(tx, 0x7D, 0xE97F);			// mic sensitivity near max (base 0xE940 | 0x3F)
		// Deviation (on-air loudness). The high nibble 0x3 is the TX-deviation mode and must stay 0x3; only the low
		// 12 bits are the deviation value (so values >= 0x4000 change the mode and kill modulation - that is why 0x5000
		// went silent). On-air the owner picked 0x3800 as the good level.
		bk4819_write(tx, 0x40, 0x3800);
		bk4819_write(tx, BK4819_REG_TX_AUDIO, 0x3B20);	// 0x50 = 0x3B20: nicFW's live-mic source value
		if (tt != BK4819_TONE_NONE) {
			// The nicFW voice base rewrote 0x51 (to 0x9100) and 0x40 (to 0x3800), wiping the sub-audio that
			// bk4819_tx_prepare() set earlier - so re-assert it LAST, after the voice registers. The mic stays live
			// (0x50 unchanged) and the CTCSS/DCS sub-tone now rides under the voice, so a repeater that needs a tone
			// opens and the far end's tone squelch hears us. 0x40 drops to the stock tone-TX deviation (headroom).
			bk4819_tx_prepare(tx, tt, tv);
		}
		bk4819_write(tx, BK4819_REG_PA, tx_pa_word(power));
		bk4819_set_freq(tx, hz);
		bk4819_write(tx, BK4819_REG_POWER, BK4819_POWER_ON);
		bk4819_set_state(tx, BK4819_STATE_TX);		// 0xC1FE: nicFW's analog-voice TX state
	} else {
		bk4819_write(tx, BK4819_REG_PA, tx_pa_word(power));
		bk4819_tx_tune(tx, hz);			// FSK burst: STATE_TX_MAIN on bus_b
	}
	tx_active = tx;
	frequency = hz;
	tx_bw = bw;
	transmitting = true;
	return true;
}
static void tx_off(void)
{
	if (!transmitting) return;
	bk4819_t *tx = tx_active ? tx_active : &receiver;
	bk4819_tx_end(tx);				// AF mute, tone off, ENABLES back to RX
	bk4819_write(tx, BK4819_REG_PA, 0);		// PA off
	bk4819_write(tx, 0x33, 0);			// drop this chip's T/R + front-end
	at32_pin_write(AT32_GPIO_F, 0, false);		// relay back to the RX path
	mic_path_tx(false);				// mux back to RX routing, mic amp off
	tx_active = NULL;
	transmitting = false;
	do_tune(frequency, tx_bw, am_mode);		// re-establish RX on the same frequency (bus_b)
	// Restore the RX AF registers the voice-TX path overwrote (do_tune does not re-run the full AF init), so receive
	// keeps working after a key-up: analog_mode resets 0x2A / 0x47 / 0x50 / 0x7E / 0x4D, and 0x28 is reset explicitly.
	bk4819_analog_mode(tx);
	nicfw_af_base(tx);				// restore the nicFW audio-DSP base (analog_mode reset parts of it) so RX works
	audio_apply();
#if CONFIG_MESSENGER
	// A voice/FSK transmit leaves the chip in plain FM RX; re-arm the analog messenger so it keeps listening (do_tune
	// above does not, since the frequency is unchanged). Only on a normal FM channel, not the AM / HF experiment.
	if (!am_mode && !hf_bk_mode)
		rt880_msg_rx_arm();
#endif
}
#if CONFIG_MESSENGER
// GOGUFW-compatible analog messenger on the channel receiver (the BK4819 does the 1.2k FSK in hardware). RX arms the
// FSK modem and polls for a GGM2 frame; TX keys the transmitter on the current frequency and sends one frame, then
// returns to RX. The receiver is shared, so FSK RX and normal FM audio do not run at the same time.
static char msg_call[MSG_CALLSIGN_LEN + 1] = "RT880";
static msg_packet_t msg_rx;
static bool msg_rx_valid;	// a valid frame has been decoded at least once (msg_rx holds the latest)
static bool msg_rx_new;		// a new frame decoded since the last rt880_msg_poll() (UI edge)
static bool msg_rx_armed;
static uint16_t msg_rx_words[MSG_FSK_WORDS];	// incremental capture buffer, drained from status()
static unsigned msg_rx_count;			// words captured so far in the frame in progress
static uint16_t msg_tx_id = 1;
// TX queue: the UI task hands a message here; the actual keying runs in rt880_msg_service() (the radio task) so only one
// task ever drives the BK4819 SPI. The flag is set last, after the text, so the radio task never reads a half-written
// buffer.
static volatile bool msg_tx_pending;
static char msg_tx_text[MSGR_TEXT_LEN + 1];
// Auto-ACK queue: when a TEXT frame is received the radio task queues an ACK back to the sender, keyed on the next
// service pass (so RX decode and TX never overlap). The sender then marks that message delivered.
static volatile bool msg_ack_pending;
static char msg_ack_to[MSG_CALLSIGN_LEN + 1];
static uint16_t msg_ack_id;

// Key one GGM2 frame already packed into w[], returning to RX afterwards. Returns whether the burst completed.
static bool msg_tx_frame(const uint16_t *w)
{
	bool was_armed = msg_rx_armed;
	bool ok = false;
	msg_rx_armed = false;
	tx_on_receiver = true;				// the messenger FSK burst keys bus_b (the RX chip), where it works
	bool keyed = transmit(frequency, tx_bw, 1, NULL);	// refused out of a TX band
	tx_on_receiver = false;
	if (keyed) {
		bk4819_fsk_setup(&receiver);
		ok = bk4819_fsk_tx(&receiver, w, MSG_FSK_WORDS);
		tx_off();
	}
	if (was_armed) {
		rt880_msg_rx_arm();			// back to listening
	}
	return ok;
}

// Perform one queued text transmit, in the radio-task context. A sent frame is copied to the Sent box (with its id so a
// later ACK can mark it delivered); one that could not be keyed (out of a TX band) goes to Drafts.
static void msg_tx_run(void)
{
	uint8_t pkt[MSG_PKT_WIRE_LEN];
	uint16_t id = msg_tx_id++;
	bool ok = false;
	if (msg_packet_build_text(pkt, sizeof pkt, id, msg_call, msg_tx_text, 3) == MSG_PKT_WIRE_LEN) {
		uint16_t w[MSG_FSK_WORDS];
		msg_fsk_pack(w, pkt);
		ok = msg_tx_frame(w);
	}
	if (ok) {
		msg_store_add_sent(msg_tx_text, id);
	} else {
		msg_store_add_draft(msg_tx_text);	// couldn't key here - keep it so the user can retry elsewhere
	}
	msg_tx_pending = false;
}

// Send the queued auto-ACK for a received message, back to its sender with the same id.
static void msg_ack_run(void)
{
	uint8_t pkt[MSG_PKT_WIRE_LEN];
	if (msg_packet_build_ack(pkt, sizeof pkt, msg_ack_id, msg_call, msg_ack_to) == MSG_PKT_WIRE_LEN) {
		uint16_t w[MSG_FSK_WORDS];
		msg_fsk_pack(w, pkt);
		(void)msg_tx_frame(w);
	}
	msg_ack_pending = false;
}

// Drain the FSK FIFO incrementally - called from status() every radio poll (25 ms), fast enough that the 8-byte FIFO
// never overruns at 1200 baud. On a completed frame it unpacks, parses, and latches the message, then restarts capture.
// Also runs any queued transmit first, so all BK4819 access stays on this (the radio) task.
void rt880_msg_service(void)
{
	if (!ready) {
		return;
	}
	if (msg_tx_pending) {
		msg_tx_run();
		return;
	}
	if (msg_ack_pending) {
		msg_ack_run();
		return;
	}
	if (transmitting || !msg_rx_armed) {
		return;
	}
	if (!bk4819_fsk_rx_service(&receiver, msg_rx_words, &msg_rx_count, MSG_FSK_WORDS)) {
		return;				// frame still in progress (or nothing to do)
	}
	uint8_t pkt[MSG_PKT_WIRE_LEN];
	if (msg_fsk_unpack(pkt, msg_rx_words) && msg_packet_parse(pkt, sizeof pkt, &msg_rx)) {
		msg_rx_valid = true;
		if (msg_rx.type == MSG_PKT_TYPE_TEXT) {
			// A text message: file it in the inbox (which also refreshes HEARD), flag the UI to pop it up, and
			// queue an ACK back to the sender so they see it was delivered.
			msg_store_add_inbox(msg_rx.from, (const char *)msg_rx.payload, msg_rx.type, last_rssi);
			msg_rx_new = true;
			unsigned i = 0;
			for (; msg_rx.from[i] && i < MSG_CALLSIGN_LEN; i++) {
				msg_ack_to[i] = msg_rx.from[i];
			}
			msg_ack_to[i] = '\0';
			msg_ack_id = msg_rx.id;
			msg_ack_pending = true;
		} else if (msg_rx.type == MSG_PKT_TYPE_ACK) {
			// An ACK for one of our messages: mark it delivered. Still refresh HEARD for the station.
			msg_store_ack(msg_rx.id);
			msg_store_heard(msg_rx.from, last_rssi, msg_rx.type);
		} else {
			msg_store_heard(msg_rx.from, last_rssi, msg_rx.type);
		}
	}
	// Re-arm for the next frame: the BK4819 FSK receiver stops after RX_FINISHED, so without this only the first
	// message after each arm was ever decoded and the rest were lost. (A pending ACK/TX re-arms again on its own.)
	msg_rx_count = 0;
	bk4819_fsk_rx_arm(&receiver);
}

void rt880_msg_set_call(const char *call)
{
	unsigned i = 0;
	for (; call != NULL && call[i] && i < MSG_CALLSIGN_LEN; i++) {
		msg_call[i] = call[i];
	}
	msg_call[i] = 0;
}

void rt880_msg_rx_arm(void)
{
	if (!ready || transmitting) {
		return;
	}
	bk4819_fsk_rx_arm(&receiver);
	msg_rx_count = 0;
	msg_rx_armed = true;
}

// UI edge: true once per newly decoded frame. The actual FIFO work happens in rt880_msg_service() from status().
bool rt880_msg_poll(void)
{
	bool was_new = msg_rx_new;
	msg_rx_new = false;
	return was_new;
}

bool rt880_msg_last(msg_packet_t *out)
{
	if (!msg_rx_valid) {
		return false;
	}
	*out = msg_rx;
	return true;
}

// Queue a message for transmission. Called from the UI task: it only copies the text and raises the pending flag; the
// radio task picks it up in rt880_msg_service() and does the actual keying, so the two tasks never touch the BK4819 at
// once. Returns false only if the modem isn't ready or a send is already queued.
bool rt880_msg_send(const char *text)
{
	if (!ready || msg_tx_pending) {
		return false;
	}
	unsigned i = 0;
	for (; text != NULL && text[i] != '\0' && i < MSGR_TEXT_LEN; i++) {
		msg_tx_text[i] = text[i];
	}
	msg_tx_text[i] = '\0';
	__asm volatile("" ::: "memory");	// publish the text before the flag the radio task reads
	msg_tx_pending = true;
	return true;
}

// Board-agnostic hooks the core messenger UI calls (messenger/msg_radio.h).
void msg_radio_set_call(const char *call) { rt880_msg_set_call(call); }
void msg_radio_rx_arm(void) { rt880_msg_rx_arm(); }
bool msg_radio_poll(void) { return rt880_msg_poll(); }
bool msg_radio_send(const char *text) { return rt880_msg_send(text); }
#endif

static void status(radio_status_t *value)
{
	*value = (radio_status_t){ .rssi_dbm = -127, .noise = 255 };
	if (!ready || !tuned) return;
	uint16_t raw = bk4819_read(&receiver, BK4819_REG_RSSI);
	value->rssi_dbm = bk4819_rssi_dbm(raw);
	value->noise = (uint8_t)(bk4819_read(&receiver, BK4819_REG_NOISE) & 0x7Fu);
	value->tone_ok = bk4819_rx_tone_ok(&receiver, bk_tone(&rx_subtone));	// CTCSS/DCS gate (always true with no tone)
	last_rssi = value->rssi_dbm;
	last_noise = value->noise;
	voice_level = bk4819_read(&receiver, 0x64);
	// Mic/voice amplitude (REG 0x64) on BOTH chips, so the diagnostic shows which BK4819 actually sees the microphone.
	voice_level_b = voice_level;
	voice_level_a = bk4819_read(&control, 0x64);
	// While transmitting voice, latch the peak mic amplitude on the TX chip, so it can be read AFTER releasing PTT:
	// a high peak means the mic ADC IS live during the real transmit (so the fault is past the ADC); ~0 means the
	// transmit config kills the ADC (unlike the no-RF probe, where it responded).
	if (transmitting) {
		bk4819_t *t = tx_active != NULL ? tx_active : &control;
		uint16_t m = (uint16_t)(bk4819_read(t, 0x64) & 0x7FFFu);
		if (m > tx_mic_peak) tx_mic_peak = m;
		// Live register readback on the TX chip, so the diagnostic shows what actually stuck during the real transmit:
		// 0x30 should read the voice-TX state, 0x40 the deviation (0 = modulation dead), 0x50 the audio source, 0x70 tone.
		tx_r30 = bk4819_read(t, 0x30);
		tx_r40 = bk4819_read(t, 0x40);
		tx_r50 = bk4819_read(t, 0x50);
		tx_r70 = bk4819_read(t, 0x58);	// FSK enable readback (bit0) - confirms the modem is off during voice TX
	}
#if CONFIG_MESSENGER
	rt880_msg_service();	// drain the FSK FIFO at the poll rate so the messenger can decode without overrun
#endif
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
	case 7: snprintf(buf, len, "mic:%04X 40:%04X", tx_mic_peak, tx_r40); break;
	case 8: snprintf(buf, len, "50:%04X 58:%04X", tx_r50, tx_r70); break;
	case 9: {
		// board.c's battery ADC (PA7, ch7). rt880_batt_raw is refreshed by the UI's periodic battery poll; show it
		// and the computed battery mV (raw*3300/4095 * 347/47) so a reading confirms the ADC works and sets the scale.
		extern uint16_t rt880_batt_raw;
		unsigned raw = rt880_batt_raw;
		unsigned mv = raw * 3300u / 4095u * 347u / 47u;
		snprintf(buf, len, "BATTraw:%u mv:%u", raw, mv);
		break;
	}
	case 10: {
		// W25Q32 data-flash probe: re-reads the JEDEC ID (a W25Q32 answers EF 40 16) so a reading confirms the SPI
		// bit-bang reaches the chip and settings storage can mount. "P:1" = present (storage can persist).
		extern uint32_t rt880_flash_jedec(void);
		uint32_t j = rt880_flash_jedec();
		snprintf(buf, len, "FLASH P:%u ID:%02X%02X%02X", (unsigned)(j >> 24) & 1u,
			 (unsigned)(j >> 16) & 0xFFu, (unsigned)(j >> 8) & 0xFFu, (unsigned)j & 0xFFu);
		break;
	}
// The Diagnostics screen stops at the first line that returns false, so a bench finder has to start right after the
// always-present line 10. RT880_PIN_FINDER and RT880_LNA_FIND are separate diagnostic images; don't combine them.
#if defined(RT880_PIN_FINDER) && !defined(RT880_LNA_FIND)
	case 11: snprintf(buf, len, "A:%04X B:%04X", at32_port_inputs(AT32_GPIO_A), at32_port_inputs(AT32_GPIO_B)); break;
	case 12: snprintf(buf, len, "C:%04X D:%04X", at32_port_inputs(AT32_GPIO_C), at32_port_inputs(AT32_GPIO_D)); break;
	case 13: snprintf(buf, len, "F:%04X (press PTT)", at32_port_inputs(AT32_GPIO_F)); break;
#endif
#ifdef RT880_LNA_FIND
	case 11: {
		// LNA / preamp finder. The RX is ~30 dB deaf (audio log: "Absolute RSSI ~30 dB low"), which points at an
		// external front-end preamp the stock enables and we don't. Every ~3 s this drives the NEXT candidate
		// front-end control and shows the live RSSI: tune a steady weak station and watch for the RSSI (and audio)
		// to jump - the label shown at that moment is the preamp's enable. "base" restores our normal state.
		// Candidates: the three known front-end-select MCU pins driven HIGH, three spare nearby pins, and extra bits
		// OR'd into the receiver's REG 0x33 (the BK4819 front-end GPIO byte) - a common place for an on-chip LNA gate.
		static const struct { const char *name; uint8_t port, pin; uint16_t r33; } cand[] = {
			{ "base", 0xFF, 0, 0 },
			{ "PC15=1", AT32_GPIO_C, 15, 0 }, { "PB11=1", AT32_GPIO_B, 11, 0 }, { "PF0=1", AT32_GPIO_F, 0, 0 },
			{ "PC14=1", AT32_GPIO_C, 14, 0 }, { "PC13=1", AT32_GPIO_C, 13, 0 }, { "PB10=1", AT32_GPIO_B, 10, 0 },
			{ "33|01", 0xFF, 0, 0x01 }, { "33|02", 0xFF, 0, 0x02 }, { "33|04", 0xFF, 0, 0x04 },
			{ "33|08", 0xFF, 0, 0x08 }, { "33|3F", 0xFF, 0, 0x3F },
		};
		enum { N = (int)(sizeof(cand) / sizeof(cand[0])) };
		static uint8_t idx;
		static uint32_t last_ms;
		uint32_t now = hal_millis();
		if (now - last_ms > 3000u || last_ms == 0) {
			last_ms = now;
			idx = (uint8_t)((idx + 1u) % N);
			// Restore our baseline first: the three front-end selects back to init (LOW outputs), the spares back to
			// floating inputs, and REG 0x33 back to route() for the tuned band.
			at32_pin_output(AT32_GPIO_C, 15, false);
			at32_pin_output(AT32_GPIO_B, 11, false);
			at32_pin_output(AT32_GPIO_F, 0, false);
			at32_pin_input_floating(AT32_GPIO_C, 14);
			at32_pin_input_floating(AT32_GPIO_C, 13);
			at32_pin_input_floating(AT32_GPIO_B, 10);
			if (started) bk4819_write(&receiver, 0x33, route(frequency));
			// Apply this candidate.
			if (cand[idx].port != 0xFF) {
				at32_pin_output(cand[idx].port, cand[idx].pin, true);
			} else if (cand[idx].r33 && started) {
				bk4819_write(&receiver, 0x33, (uint16_t)(route(frequency) | cand[idx].r33));
			}
		}
		snprintf(buf, len, "LNA %s R:%d", cand[idx].name, last_rssi);
		break;
	}
#endif
	default: return false;
	}
	return true;
}
