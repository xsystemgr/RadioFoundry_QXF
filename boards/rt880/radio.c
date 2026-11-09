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
// BK4819 REG 0x36 is 16-bit: the HIGH byte is the PA bias - the current into the final that actually sets the output
// power - and the LOW byte is the enable bit (7) plus the gain select. The stock radio keys TX with (bias << 8) | 0xFF
// and takes the bias from per-band calibration. An earlier version wrote only the low byte, leaving the bias at 0, so
// the PA was "on" but radiated nothing (confirmed on hardware: TX stored to Sent yet no carrier). These are deliberately
// conservative lab bias levels, well under full scale (0xFF high byte). Transmit into a proper antenna or dummy load.
static uint16_t tx_pa_word(uint8_t power)
{
	// First-light bias levels, kept deliberately LOW while the RF antenna path is validated on hardware - just enough
	// to confirm a carrier, far below the stock per-band calibration. Raise only once the path is proven and into a
	// proper antenna / dummy load.
	uint16_t bias = power >= 1 ? 0x18u : 0x10u;	// HI / LO (minimal)
	return (uint16_t)((bias << 8) | 0xFFu);		// bias in the high byte, enable + full gain select in the low
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
	// Faithful stock (V1.16) TX antenna / front-end state, reverse-engineered from the factory rf_path_set + T/R
	// control. For both ham bands the RF front-end selects PC15 and PB11 are driven LOW and the band relay PF0 is LOW;
	// BK4819 REG 0x33 selects the front-end filter (VHF = bit 3 = 0x08, UHF = bit 2 = 0x04) OR'd with the antenna T/R
	// bit (0x10), which the stock raises only while transmitting. The earlier build never drove PC15/PB11 and toggled
	// PF0 the wrong way, so the antenna never reached the PA output (only a relay click, no carrier).
	bool uhf = hz >= 400000000u;
	at32_pin_write(AT32_GPIO_C, 15, false);
	at32_pin_write(AT32_GPIO_B, 11, false);
	at32_pin_write(AT32_GPIO_F, 0, false);
	bk4819_write(&receiver, 0x33, (uint16_t)((uhf ? 0x04u : 0x08u) | 0x10u));
	// Silence the tone generators so the microphone modulates the carrier. The analog messenger leaves Tone2 enabled
	// (REG 0x70 = 0x00E0) for its FSK, and it is kept armed on every FM channel - without clearing it here a voice TX
	// would modulate with Tone2 (nothing) instead of the mic. The messenger's own FSK TX re-enables Tone2 afterwards.
	bk4819_write(&receiver, 0x70, 0x0000);
	bk4819_tx_prepare(&receiver, tt, tv);		// deviation and sub-audio
	bk4819_write(&receiver, BK4819_REG_PA, tx_pa_word(power));
	bk4819_tx_tune(&receiver, hz);			// freq, POWER_ON, STATE_TX_MAIN (mic path on)
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
	if (transmit(frequency, tx_bw, 1, NULL)) {	// key TX (refused out of a TX band)
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
	value->tone_ok = true;
	last_rssi = value->rssi_dbm;
	last_noise = value->noise;
	voice_level = bk4819_read(&receiver, 0x64);
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
