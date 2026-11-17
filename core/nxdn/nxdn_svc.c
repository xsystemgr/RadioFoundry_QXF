/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "nxdn_svc.h"
#include "nxdn_link.h"
#include "dmr/ambe/ambeVoice.h"

/* One link instance (RX). Its demod/call state is the bulk of the RAM this feature adds. */
static nxdn_link s_link;
static bool s_active;

/* Each decoded 72-bit NXDN voice slot -> the shared DMR AMBE voice sink. ambeVoice opens a call on the first frame and
 * handles priming / concealment, exactly as it does for DMR, because the air frame is identical. */
static void slot_to_ambe(void *ctx, const uint8_t slot[9])
{
	(void)ctx;
	const dmr_voice_sink_t *sink = ambeVoiceSink();
	if (sink != NULL && sink->frame != NULL)
		sink->frame(slot, sink->ctx);
}

void nxdn_svc_rx_start(const hal_audio_t *audio, const hal_vocoder_t *vocoder,
		       uint8_t ran, uint16_t group, uint16_t unit)
{
	ambeVoiceInit(audio, vocoder);
	/* A flat audio path for now; the board can pass its measured FM RX corners (hpf / de-emphasis) once characterised,
	 * which only sharpens weak-signal decode. */
	static const nxdn_audio_path path = { 0, 0, 0, 0 };
	nxdn_link_init(&s_link, &path, ran, group, unit, slot_to_ambe, NULL);
	s_active = true;
}

void nxdn_svc_rx_audio(const int16_t *pcm, size_t n)
{
	if (s_active)
		nxdn_link_rx_audio(&s_link, pcm, n);
}

void nxdn_svc_rx_restart(void)
{
	if (s_active)
		nxdn_link_rx_restart(&s_link);
}

void nxdn_svc_stop(void)
{
	if (s_active) {
		ambeVoiceStop();
		s_active = false;
	}
}

bool nxdn_svc_active(void)
{
	return s_active;
}
