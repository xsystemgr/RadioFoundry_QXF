// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// TYT MD-2017 / Retevis RT82: board descriptor, key map and what sets the radio apart in the UV380 family
// (boards/uv380_family/uvf.h with UVF_MD2017, docs/spec/hardware-md2017.md). Bring-up: receiver A only, the UV380 family's
// keypad, calibration and power tables until each is checked on the radio. Receiver B is dual watch's second
// receiver for FM (radio_uvf.c, uvf_rx2)
#include "board.h"
#include "config.h"
#include "../uv380_family/uvf.h"
#include "hrc/dmr_glue.h"
#include "hrc/vocoder.h"
#include "pins.h"
#include "fsmc_keypad/fsmc_keypad.h"
#if CONFIG_USB
#include "usb/usb_device.h"
#include "usb/usb_ncm.h"
#endif
#if CONFIG_NET_PPP
#include "hostlink/hostlink_ppp.h"
#endif

// Transmit: on since the first test on the radio (2026-10-30: FM and DMR out, the power steps as set);
// MD2017_DEFS="-DMD2017_TX=0" builds a receive-only image
#ifndef MD2017_TX
#define MD2017_TX		1
#endif

// The UV380 family's other radios' matrix, unconfirmed here: the keys pressed show on the diagnostics page
const uint8_t uvf_key_matrix[3][8] = {
	{ KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_0, KEY_STAR },
	{ KEY_GREEN, KEY_UP, KEY_DOWN, KEY_7, KEY_8, KEY_9, KEY_HASH, KEY_RED },
	{ STM32_KEYPAD_NO_KEY, STM32_KEYPAD_NO_KEY, STM32_KEYPAD_NO_KEY, STM32_KEYPAD_NO_KEY, STM32_KEYPAD_NO_KEY,
	  STM32_KEYPAD_NO_KEY, KEY_SK2, KEY_SK1 },
};

static const char *tx_block(void)
{
	return MD2017_TX ? NULL : "MD-2017 bring-up: RX only";
}

// The inputs the TYT image reads whose use isn't known yet (pins.h): configured as it does, PA13 and PA15 read as
// they are (SWD / JTAG)
static void init(void)
{
	stm32_pin_in(PIN_PORT(PIN_IN_PB0), PIN_MASK(PIN_IN_PB0) | PIN_MASK(PIN_IN_PB1), GPIO_NOPULL);
	stm32_pin_in(PIN_PORT(PIN_IN_PB10), PIN_MASK(PIN_IN_PB10), GPIO_PULLUP);
	stm32_pin_in(PIN_PORT(PIN_IN_PD10), PIN_MASK(PIN_IN_PD10) | PIN_MASK(PIN_IN_PD11), GPIO_NOPULL);
	stm32_pin_in(PIN_PORT(PIN_IN_PE12), PIN_MASK(PIN_IN_PE12) | PIN_MASK(PIN_IN_PE13) | PIN_MASK(PIN_IN_PE15),
		     GPIO_PULLUP);
}

#if CONFIG_DIAG
// Three lines "A13:1 A15:1 B0:0 B1:0": each unknown input's level (1 = high), for finding what each one is
static bool diag_line(uint8_t i, char *buf, uint8_t len)
{
	static const struct { GPIO_TypeDef *port; uint16_t pin; const char *name; } in[] = {
		{ PIN_IN_PA13, "A13" }, { PIN_IN_PA15, "A15" }, { PIN_IN_PB0, "B0" }, { PIN_IN_PB1, "B1" },
		{ PIN_IN_PB10, "B10" }, { PIN_IN_PD10, "D10" }, { PIN_IN_PD11, "D11" }, { PIN_IN_PE11, "E11" },
		{ PIN_IN_PE12, "E12" }, { PIN_IN_PE13, "E13" }, { PIN_IN_PE15, "E15" },
	};
	const uint8_t per = 4, count = sizeof in / sizeof in[0];
	uint8_t n = 0;
	if (len < 2u) {
		return false;
	}
#if CONFIG_DUAL_WATCH
	if (i == (count + per - 1u) / per) {
		uvf_rx2_diag(buf, len);	// receiver B, after the pin lines
		return true;
	}
#endif
	if (i * per >= count) {
		return false;
	}
	for (uint8_t k = i * per; k < count && k < (i + 1u) * per; k++) {
		for (const char *p = in[k].name; *p != '\0' && n + 4u < len; p++) {
			buf[n++] = *p;
		}
		if (n + 3u < len) {
			buf[n++] = ':';
			buf[n++] = (in[k].port->IDR & in[k].pin) ? '1' : '0';
			buf[n++] = ' ';
		}
	}
	buf[n] = '\0';
	return true;
}
#endif

// The UV380 family's 5 W rows: placeholders until the MD-2017's are measured
const uvf_variant_t uvf_variant = {
	.panel_type = { 0, 1, 2, 3 },
	.panel_rgb = { true, true, true, true },
	.bat_mul = 1000, .bat_div = 403,
	.vol_min = 30, .vol_break = 313, .vol_max = 2020,
	.frac = { { 350, 700, 340, 610 }, { 400, 700, 250, 550 } },
	.cal_k = { { 630, 1000, 1000, 1100 }, { 720, 1000, 1050, 1250 } },
	.init = init,
	.tx_block = tx_block,
#if CONFIG_DIAG
	.diag_line = diag_line,
#endif
};

static const band_t bands[] = {
	{ 136000000, 174000000, true },
	{ 400000000, 480000000, true },
};

static const char *const power_names[] = { "50mW", "250mW", "500mW", "750mW", "1W", "2W", "3W", "4W", "5W" };

// docs/ux/information-architecture.md 7.1 on a radio without function or top keys (a few-key map): the zone and power
// are reached from the menus. SK1 and SK2 as on the family's other radios until the MD-2017's matrix is
// read on the bench (hardware-md2017.md 3.1)
static const keymap_entry_t keymap[] = {
	{ KEY_GREEN, ACT_SELECT, ACT_SELECT, false, false, 0 },
	{ KEY_RED, ACT_BACK, ACT_BACK, false, false, 0 },
	{ KEY_UP, ACT_NAV_UP, ACT_NAV_UP, false, true, 0 },
	{ KEY_DOWN, ACT_NAV_DOWN, ACT_NAV_DOWN, false, true, 0 },
	{ KEY_STAR, ACT_QUICK, ACT_QUICK, false, false, '*' },
	{ KEY_HASH, ACT_CONTEXT, ACT_CONTEXT, false, false, '#' },
	{ KEY_0, ACT_DIGIT_0, ACT_DIGIT_0, false, false, 0 },
	{ KEY_1, ACT_DIGIT_1, ACT_DIGIT_1, false, false, 0 },
	{ KEY_2, ACT_DIGIT_2, ACT_DIGIT_2, false, false, 0 },
	{ KEY_3, ACT_DIGIT_3, ACT_DIGIT_3, false, false, 0 },
	{ KEY_4, ACT_DIGIT_4, ACT_DIGIT_4, false, false, 0 },
	{ KEY_5, ACT_DIGIT_5, ACT_DIGIT_5, false, false, 0 },
	{ KEY_6, ACT_DIGIT_6, ACT_DIGIT_6, false, false, 0 },
	{ KEY_7, ACT_DIGIT_7, ACT_DIGIT_7, false, false, 0 },
	{ KEY_8, ACT_DIGIT_8, ACT_DIGIT_8, false, false, 0 },
	{ KEY_9, ACT_DIGIT_9, ACT_DIGIT_9, false, false, 0 },
	{ KEY_SK1, ACT_MONITOR, ACT_MONITOR, true, false, 0 },
	{ KEY_SK2, ACT_AB, ACT_USER_2, false, false, 0 },
	{ KEY_PTT, ACT_PTT, ACT_PTT, true, false, 0 },
};

// Reassignable in Settings > Keys and the codeplug (core/input/keyassign.h): the side keys top to bottom
static const uint8_t side_keys[] = { KEY_SK1, KEY_SK2 };
static const char *const side_key_names[] = { "SK1", "SK2" };

#if CONFIG_CPS_OPENFW
// open-firmware-cps.md 4: type 6, the UV380 family (the MD-2017 has no type of its own yet, ?)
static uint32_t cps_radio_type(void)
{
	return 6;
}

static const hal_cps_t cps = {
	.radio_type = cps_radio_type,
	.rom_base = 0x08000000u,
	.rom_size = 1024u * 1024u,
	.security_read = stm32_spi_flash_cps_security_read,
};
#endif

const board_t board = {
	.name = "MD-2017",
	.caps = CAP_KEYPAD | CAP_ENCODER | CAP_VOLUME_POT | (CONFIG_GPS ? CAP_GPS : 0) | (CONFIG_DMR ? CAP_DMR : 0),
	.bands = bands,
	.band_count = sizeof(bands) / sizeof(bands[0]),
	.power_count = sizeof(power_names) / sizeof(power_names[0]),
	.power_names = power_names,
	.power_default = 4,	// 1 W
	.display = &uvf_display,
	.input = &uvf_input,
	.keymap = keymap,
	.keymap_len = sizeof(keymap) / sizeof(keymap[0]),
	.side_keys = side_keys,
	.side_key_names = side_key_names,
	.side_key_count = sizeof(side_keys),
	.radio = &uvf_radio,
#if CONFIG_DUAL_WATCH
	.rx2 = &uvf_rx2,	// receiver B: both FM channels of dual watch at once (radio_uvf.c)
#endif
	.power = &uvf_power,
	.flash = &stm32_spi_flash,
#if CONFIG_DIAG
	.diag_line = uvf_diag_line,
	.diag_reset = uvf_diag_reset,
#endif
#if CONFIG_DMR
	.dmr = &stm32_hrc_dmr,
	.audio = &uvf_audio,
	.vocoder = &stm32_vocoder,
#if CONFIG_AUDIO_PROBE || CONFIG_NET_USRP || CONFIG_APRS_RX
	.audio_tap = &uvf_audio_tap,
#endif
#endif
#if CONFIG_USB
	.host_port = &usb_cdc_stream,
#if CONFIG_NET_PPP
	.netif = &hostlink_ppp_netif,	// PPP on the USB serial port, the network adapter under it (board_init)
#elif CONFIG_USB_NETWORK
	.netif = &usb_ncm_netif,
#endif
#endif
	.system = &stm32_system,
#if CONFIG_GPS
	.gps_present = stm32_gps_present,
#endif
	// The family's thresholds, unconfirmed on this radio: low battery 6.2 V, critical 6.0 V
	.battery_low_mv = 6200,
	.battery_critical_mv = 6000,
#if CONFIG_CPS_OPENFW
	.cps = &cps,
#endif
#if CONFIG_APRS
	.afsk = &uvf_afsk,
#endif
};
