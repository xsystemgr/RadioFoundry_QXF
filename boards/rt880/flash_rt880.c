// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// RT-880 data flash: the on-board Winbond W25Q32 SPI NOR (4 MB, 4 KB sectors, 256-byte pages), confirmed from the
// schematic (U on page 2: W25Q-CS/DI/DO/SCK). The stock firmware drives it over the AT32's hardware SPI2; we bit-bang
// the same four pins with the GPIO HAL, matching how every other bus on this board is driven (BK4819, ST7789, Si4732).
// SPI mode 0. Only the core's settings ring + journal live here, placed high in the device to stay clear of whatever
// the stock keeps low (codeplug / calibration); the native codeplug is left in RAM/defaults (native_size = 0), so a
// bad write here cannot corrupt channels.
#include "board.h"
#include "config.h"
#include "platform.h"
#include <stdint.h>
#include <stdbool.h>

#define PORT	AT32_GPIO_B
#define PIN_CS	12u	// W25Q-CS  (active low)
#define PIN_SCK	13u	// W25Q-SCK
#define PIN_DO	14u	// W25Q-DO  -> MCU input  (flash MISO)
#define PIN_DI	15u	// W25Q-DI  -> MCU output (flash MOSI)

#define FLASH_SIZE	0x400000u	// W25Q32: 4 MB
#define SECTOR		4096u
#define PAGE		256u

// Commands
#define CMD_RDID	0x9Fu
#define CMD_READ	0x03u
#define CMD_PP		0x02u
#define CMD_SE		0x20u	// 4 KB sector erase
#define CMD_WREN	0x06u
#define CMD_RDSR	0x05u	// status: bit0 = WIP (busy)

static bool present;

static void cs(bool select)	{ at32_pin_write(PORT, PIN_CS, !select); }	// CS is active low

// One SPI byte, mode 0: MOSI set while clock low, flash samples on the rising edge, we sample MISO on the same edge.
static uint8_t spi_xfer(uint8_t out)
{
	uint8_t in = 0;
	for (int i = 7; i >= 0; i--) {
		at32_pin_write(PORT, PIN_DI, (out >> i) & 1u);
		at32_pin_write(PORT, PIN_SCK, true);
		in = (uint8_t)((in << 1) | (at32_pin_read(PORT, PIN_DO) ? 1u : 0u));
		at32_pin_write(PORT, PIN_SCK, false);
	}
	return in;
}

static void send_addr(uint8_t cmd, uint32_t a)
{
	spi_xfer(cmd);
	spi_xfer((uint8_t)(a >> 16));
	spi_xfer((uint8_t)(a >> 8));
	spi_xfer((uint8_t)a);
}

static void write_enable(void)
{
	cs(true);
	spi_xfer(CMD_WREN);
	cs(false);
}

// Poll the status register until WIP clears, bounded so a missing/stuck flash can never hang the radio.
static bool wait_ready(void)
{
	cs(true);
	spi_xfer(CMD_RDSR);
	bool ok = false;
	for (uint32_t i = 0; i < 2000000u; i++) {
		if ((spi_xfer(0xFFu) & 0x01u) == 0u) { ok = true; break; }
	}
	cs(false);
	return ok;
}

static bool rt880_flash_init(void)
{
	at32_pin_output(PORT, PIN_CS, true);	// deselect
	at32_pin_output(PORT, PIN_SCK, false);	// clock idle low (mode 0)
	at32_pin_output(PORT, PIN_DI, false);
	at32_pin_input_floating(PORT, PIN_DO);
	// Read the JEDEC ID; a W25Q32 answers EF 40 16. Accept any sane, non-floating answer so a compatible 25-series
	// part still works, but reject all-ones / all-zeros (no chip / stuck bus).
	cs(true);
	uint8_t mfr = spi_xfer(CMD_RDID), t = spi_xfer(0xFFu), cap = spi_xfer(0xFFu);
	cs(false);
	present = !(mfr == 0xFFu && t == 0xFFu && cap == 0xFFu) && !(mfr == 0 && t == 0 && cap == 0);
	return present;
}

// Diagnostics helper: (re)configure the pins, re-read the JEDEC ID, and report presence. Packed as
// (present<<24)|(mfr<<16)|(type<<8)|capacity so a single diag line can show it. A W25Q32 answers EF 40 16.
uint32_t rt880_flash_jedec(void)
{
	rt880_flash_init();
	cs(true);
	uint8_t mfr = spi_xfer(CMD_RDID), t = spi_xfer(0xFFu), cap = spi_xfer(0xFFu);
	cs(false);
	return ((uint32_t)(present ? 1u : 0u) << 24) | ((uint32_t)mfr << 16) | ((uint32_t)t << 8) | cap;
}

static bool rt880_flash_read(uint32_t addr, void *buf, uint32_t len)
{
	if (!present || addr + len > FLASH_SIZE) return false;
	uint8_t *p = buf;
	cs(true);
	send_addr(CMD_READ, addr);
	for (uint32_t i = 0; i < len; i++) p[i] = spi_xfer(0xFFu);
	cs(false);
	return true;
}

// Program already-erased bytes, any length/alignment, split on 256-byte page boundaries (a page program wraps within
// its page otherwise).
static bool rt880_flash_write(uint32_t addr, const void *buf, uint32_t len)
{
	if (!present || addr + len > FLASH_SIZE) return false;
	const uint8_t *p = buf;
	while (len > 0) {
		uint32_t room = PAGE - (addr % PAGE);
		uint32_t n = len < room ? len : room;
		write_enable();
		cs(true);
		send_addr(CMD_PP, addr);
		for (uint32_t i = 0; i < n; i++) spi_xfer(p[i]);
		cs(false);
		if (!wait_ready()) return false;
		addr += n; p += n; len -= n;
	}
	return true;
}

static bool rt880_flash_erase(uint32_t addr)
{
	if (!present || addr >= FLASH_SIZE) return false;
	write_enable();
	cs(true);
	send_addr(CMD_SE, addr & ~(SECTOR - 1u));
	cs(false);
	return wait_ready();
}

// Settings ring + journal placed high in the 4 MB device, away from the stock's low codeplug/calibration. No
// open-firmware images, no calibration window, no native codeplug in flash (channels stay on defaults in RAM), no
// staging - so nothing here can clobber channels or the stock calibration.
static const hal_flash_map_t map = {
	.settings_addr = 0x3F0000u,	// 8 sectors (32 KB) for the settings ring
	.settings_size = 0x008000u,
	.journal_addr  = 0x3F8000u,	// 2 sectors for the sector journal
};

const hal_flash_t rt880_flash = {
	.init = rt880_flash_init,
	.size = FLASH_SIZE,
	.erase_size = SECTOR,
	.read = rt880_flash_read,
	.write = rt880_flash_write,
	.erase = rt880_flash_erase,
	.replace = NULL,
	.map = &map,
};
