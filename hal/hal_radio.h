// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Radio: what the core asks of the RF side, whatever chips the board has.
// The board composes its RF chip, modem / audio chip, band and PA switching and calibration behind this.
#ifndef HAL_RADIO_H
#define HAL_RADIO_H

#include <stdint.h>
#include <stdbool.h>

typedef enum { RADIO_MODE_FM, RADIO_MODE_DMR } radio_mode_t;

// hal_radio_t.am_mode (radio_settings_t.am_mode; airband-and-broadcast.md 3.5): the radio's own firmware's AM set-up
// (the default); the AF output type AM alone with the AGC automatic, or fixed at a high or a middle gain index; slope
// detection (the FM demodulator 1.6 kHz off the carrier, the 25 kHz filter, the AGC fixed); or an AM demodulator
// enable bit with the normal receive AF output
#define RADIO_AM_MODE_STOCK		0u
#define RADIO_AM_MODE_NATIVE		1u
#define RADIO_AM_MODE_AGC_HIGH		2u
#define RADIO_AM_MODE_AGC_MID		3u
#define RADIO_AM_MODE_SLOPE		4u
#define RADIO_AM_MODE_ENABLE_BIT	5u
#define RADIO_AM_MODE_COUNT		6u
typedef enum { RADIO_BW_12K5, RADIO_BW_25K } radio_bw_t;

typedef enum { TONE_NONE, TONE_CTCSS, TONE_DCS, TONE_DCS_INV } tone_type_t;
typedef struct {
	uint8_t type;		// tone_type_t
	uint16_t value;		// CTCSS: Hz x 10 (1000 = 100.0 Hz). DCS: the code as written, octal digits (023 -> 0x13 = 19 = 023 octal)
} subtone_t;

typedef struct {
	int16_t rssi_dbm;
	uint8_t noise;		// chip noise reading, larger = more noise (1 dB units where the chip allows)
	bool carrier;		// the chip's own squelch says carrier (if it has one)
	bool tone_ok;		// the RX sub-audio tone or code matches (true when none is set)
} radio_status_t;

typedef struct {
	void (*init)(void);
	void (*set_mode)(radio_mode_t mode);
	// Tune and receive. Out-of-band frequencies are refused (false)
	bool (*rx)(uint32_t hz, radio_bw_t bw, const subtone_t *rx_tone);
	// Transmit FM (or key up for DMR) on hz at a board power level index. False: not allowed here
	bool (*tx)(uint32_t hz, radio_bw_t bw, uint8_t power, const subtone_t *tx_tone);
	// Back to receive with the last rx() settings
	void (*tx_off)(void);
	void (*status)(radio_status_t *st);
	// Speaker path (amp and mute) for received audio
	void (*audio)(bool on);
	// Optional: received audio level 0..100 % (radios whose volume knob is read by the MCU, not wired to the amp)
	void (*volume)(uint8_t percent);
	// With volume: true when that level also scales the PCM speaker path (beeps, DMR voice). False: beeps on the PCM
	// path would ignore it, so the core caps each beep at the volume percent (radio_beep_level)
	bool volume_pcm;
	// Optional: during FM TX, send a single audio tone of hz instead of the microphone (1750 Hz burst, roger beep);
	// 0 = the microphone again. False when the board can't (not in FM TX)
	bool (*tx_tone)(uint16_t hz);
	// Optional, with tx_tone: key the tone on and off without giving the microphone back (Morse keying, the hotspot's
	// CW ID): false = a silent carrier, true = the tone again. Only while a tone set with tx_tone runs. Without it the
	// core keys with tx_tone(hz) / tx_tone(0), which lets the microphone through in the gaps
	bool (*tx_tone_key)(bool down);
	// Optional (CONFIG_DTMF): during FM TX, send the DTMF pair of digit ('0'-'9', '*', '#', 'A'-'D'; hal_dtmf.h)
	// instead of the microphone until called again; 0 = the microphone again. A new digit replaces the one sounding.
	// False when the board can't (not in FM TX, not a digit). The core never mixes it with tx_tone
	bool (*tx_dtmf)(char digit);
	// Optional: the turn-off tail of the TX tone tx() set, at the end of an FM transmission (DCS: the 134.4 Hz tone that
	// closes the receivers' squelch before the carrier drops). on: start it; false when there is none (no DCS, or the
	// chip can't), and the core unkeys at once. off: stop it, still keyed. The core keeps the carrier up
	// RADIO_SUBTONE_TAIL_MS between the two, then calls tx_off(), which also ends a tail still running
	bool (*tx_tail)(bool on);
	// Optional: receiver power saving. true = receiver off (no status until woken), false = receive again with the
	// last rx() settings
	void (*rx_sleep)(bool sleep);
	// Optional (CAP_AM_RX): tune and receive AM (the air band) instead of FM. Receive only: the core never calls tx()
	// while AM is on. The board leaves AM at the next rx() or set_mode(). No sub-audio in AM; status() reports
	// tone_ok = true and an RSSI the core's AM squelch uses (noise isn't meaningful in AM). False: out of band or the
	// board can't. NULL = no AM on this radio
	bool (*rx_am)(uint32_t hz, radio_bw_t bw);
	// Optional: the microphone gain of FM and DMR transmissions, db relative to the board's own levels (the Mic gain
	// setting, -10..+10; core/radio/mic_gain.h). The board only stores it and applies it at the next transmission
	// start, so any task may call it. NULL = no gain in the board's audio path: DMR then scales the samples before the
	// vocoder and FM stays as it is
	void (*mic_gain)(int8_t db);
	// Optional (CAP_AM_RX, experimental): how rx_am() demodulates, RADIO_AM_MODE_* below (Settings > Talk > AM demod).
	// The board applies it at once when AM is on, else at the next rx_am(). NULL = one way only
	void (*am_mode)(uint8_t mode);
} hal_radio_t;

#endif
