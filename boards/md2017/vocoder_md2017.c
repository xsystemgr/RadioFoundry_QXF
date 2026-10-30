// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// MD-2017 vocoder: the radio's own AMBE+2 codec run in place from flash (docs/spec/codec-placement.md, "STM32 radios";
// the codec: docs/spec/hardware-md2017.md 1.1). The donor is TYT's V4.05 file. The merge on the user's machine
// (rfoundry) writes the codec's tables at their address in that build (cut to the codec's own data), its code right
// after them, word aligned, and the descriptor at the end of sector 11 (platform/stm32f4/codec_desc.h). The code
// reaches its tables through absolute addresses, which don't change; the merge rewrites one pointer into the code and
// the 12 calls from the code kept in the table window back into the code (codec map md2017/md2017-v405). Without a
// valid descriptor and codec, present() is false and the radio is FM only.
#include "../uv380_family/uvf.h"
#include "config.h"

#if CONFIG_DMR	// the whole file: DMR only
#include "hrc/vocoder.h"

#define CODEC_FLASH_LO		0x080C0000u	// sectors 10 and 11: the flash boards/md2017/memory.ld leaves to the codec

// The id tools/rfoundry uses for the codec (stockfw.py MD2017_CODEC)
#define CODEC_ID		"md2017-v405-s11"

// Entry offsets from the code start: the UV380 family's codec layout (codec-mduv380.md), checked for V4.05 in an
// emulator (hardware-md2017.md 1.1)
#define CODEC_ENTRIES { .encode = 0x130, .decode = 0x5D8, .enc_init = 0x284, .fec_init = 0x4CC, .dec_init = 0x6D0, \
		.enc_params = 0x848, .enc_set = 0x1C0, .dec_set = 0x688 }

// V4.05 UVCSV as the merge places it (stockfw.py "placed"): tag, code and tables with the CRC-32 of each
static const codec_desc_window_t v405_tables[] = { { 0x080D2AF4u, 0x11F56u, 0x08F19F1Du } };
static const stm32_codec_profile_t v405 = {
	.tag = 0x00550405u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = 0x080E4A4Cu, .code_len = 0xF6E2u, .code_crc = 0xA0E05F0Cu, .tables = v405_tables, .windows = 1,
	.at = CODEC_ENTRIES,
};

static const stm32_codec_profile_t *const profiles[] = { &v405 };
const stm32_codec_profiles_t stm32_codec_profiles = { profiles, (uint8_t)(sizeof profiles / sizeof profiles[0]) };

#endif	// CONFIG_DMR
