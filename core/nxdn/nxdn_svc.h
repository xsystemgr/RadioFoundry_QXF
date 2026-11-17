/* SPDX-License-Identifier: GPL-3.0-or-later */
/* NXDN48 radio service: ties the portable NXDN link (FM-audio software 4FSK + NXDN call layer) to the shared DMR AMBE
 * voice path. NXDN carries the same 72-bit AMBE+2 air frame as DMR, so decoded voice slots go straight into
 * ambeVoice's sink (FEC -> vocoder -> speaker) with no new codec glue.
 *
 * The board drives it: on an NXDN channel it puts the radio in FM RX, then hands each 8 kHz FM-RX audio block to
 * nxdn_svc_rx_audio(). RX only for now; TX waits on the HR_C6000 FM-modulation-source work (modem-hrc6000.md 8.1). */
#ifndef NXDN_SVC_H
#define NXDN_SVC_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "hal_audio.h"
#include "hal_vocoder.h"

/* Start NXDN RX for a channel's RAN / talkgroup. audio + vocoder are the board's (the same ones DMR uses). */
void nxdn_svc_rx_start(const hal_audio_t *audio, const hal_vocoder_t *vocoder,
		       uint8_t ran, uint16_t group, uint16_t unit);

/* Feed one block of 8 kHz, 16-bit FM-RX discriminator audio (any sample count). Call from the board's audio task. */
void nxdn_svc_rx_audio(const int16_t *pcm, size_t n);

/* Drop any partial sync and re-acquire (e.g. after retune or squelch close). */
void nxdn_svc_rx_restart(void);

/* Leave NXDN: stop the voice stream and release the audio path. */
void nxdn_svc_stop(void);

bool nxdn_svc_active(void);

#endif
