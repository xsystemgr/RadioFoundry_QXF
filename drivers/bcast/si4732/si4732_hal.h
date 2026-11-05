// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The Si4732 as the board's hal_bcast_t: chip driver (si4732.h) and a bit-banged I2C bus on two GPIO pins
// (drivers/bus/i2c_gpio) behind the interface the core's broadcast receiver uses (hal_bcast.h). On the RT-880 this is
// the HF/AM second receiver: the VFO tuned into its range (set by lo_khz/hi_khz) is heard on it, receive only. A board
// supplies its pins and, if it has them, the tuner's power line and the audio path switch; one tuner per radio, so the
// state is static. All calls come from one task.
#ifndef SI4732_HAL_H
#define SI4732_HAL_H

#include "hal_bcast.h"
#include "si4732.h"
#include "bus/i2c_gpio/i2c_gpio.h"

// The range this receiver tunes, fixed per build like the RDA5807M's. The default is HF: LW/MW/SW up to 30 MHz, on a
// 1 kHz grid (fine enough for shortwave broadcast and for landing on an AM-demodulated voice frequency). A board that
// fits the chip for FM broadcast instead can override these with -D.
#ifndef SI4732_LO_KHZ
#define SI4732_LO_KHZ	150u
#endif
#ifndef SI4732_HI_KHZ
#define SI4732_HI_KHZ	30000u
#endif
#ifndef SI4732_GRID_KHZ
#define SI4732_GRID_KHZ	1u
#endif

typedef struct {
	const i2c_gpio_t *bus;		// the chip's two pins (SCL/SDA); half_us 5 is 100 kHz
	uint8_t addr7;			// the chip's 7-bit I2C address (Si4732: 0x11 with SEN low, 0x63 with SEN high)
	void (*delay_ms)(uint32_t ms);	// the chip's power-up/reset time; may sleep
	void (*enable)(bool on);	// optional: the tuner's power / reset line, called before power-up and after power-down
	void (*audio_path)(bool on);	// optional: the board's switch that sends the tuner's audio to the speaker
	uint32_t start_khz;		// the frequency the first power-up tunes to
} si4732_hal_cfg_t;

// Select the board's wiring. cfg must stay valid. Call once, before the core uses si4732_hal
void si4732_hal_init(const si4732_hal_cfg_t *cfg);

// One-shot bench probe: power the chip, read GET_REV, then power it back down (so the core's own power()/tune() are
// unaffected). part is the chip's part number (Si4732 reports 32), fw_* its firmware revision. False when nothing on
// the bus acknowledged - the I2C pins, the address or the power line are wrong. For a board diagnostic line.
bool si4732_hal_probe(uint8_t *part, uint8_t *fw_major, uint8_t *fw_minor);

// The last status the core read from the chip (updated every status() call), for a live diagnostic line. Returns
// false when the chip is not powered. *st is the most recent reading: khz tuned, rssi, station flag, tuning flag.
bool si4732_hal_last(bcast_status_t *st);

// How HF is demodulating now: "AM", "LSB" or "USB", for a diagnostic line.
const char *si4732_hal_demod_name(void);

extern const hal_bcast_t si4732_hal;

// The command transport over the I2C master, for hosts that want the chip without the HAL (and for tests)
extern const si4732_bus_t si4732_i2c_bus;

#endif
