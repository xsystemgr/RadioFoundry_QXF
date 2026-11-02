// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Pin-level RF model: independent serial buses, direction changes and stage guards.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../radio.h"
#include "platform.h"
#include "reset_policy.h"
#include "rf/bk4819/bk4819.h"

typedef struct {
	bool cs, driving, reading;
	uint32_t shift;
	unsigned bits, read_bits, writes, output_setups;
	uint16_t regs[128], id, response;
} chip_t;
static chip_t chips[2];
static bool pins[6][16], panic;
static unsigned gpio_writes, critical_depth, frames;

static void select_chip(unsigned index, bool high)
{
	chip_t *c = &chips[index];
	if (c->cs && !high) {
		assert(chips[1-index].cs);
		c->bits = c->shift = c->read_bits = 0;
		c->reading = false;
	}
	if (!c->cs && high && !c->reading) {
		assert(c->bits == 24);
		unsigned reg = (c->shift >> 16) & 0x7F;
		uint16_t value = (uint16_t)c->shift;
		if (reg == 0x36) assert(value == 0);
		if (reg == 0x30) {
			assert((value & 0x400Eu) == 0); // PA gain, microphone ADC and TX DSP bits stay clear
			if (index == 0) assert(value == 0); // bus_a is the idle control chip
		}
		if (reg == 0x33) {
			// Band-select GPIO is written on the receiver (bus_b = index 1).
			assert(index == 1 ? value == 0 || value == 0x10 || value == 0x20 : value == 0);
		}
		if (reg != 0) c->regs[reg] = value;
		c->writes++; frames++;
	}
	if (high) c->reading = false;
	c->cs = high;
}
void at32_pin_write(uint32_t port, unsigned pin, bool high)
{
	unsigned bank = (port - AT32_GPIO_A) / 0x400;
	bool rising = !pins[bank][pin] && high;
	pins[bank][pin] = high;
	gpio_writes++;
	if (port == AT32_GPIO_C && pin == 7) select_chip(0, high);
	if (port == AT32_GPIO_A && pin == 8) select_chip(1, high);
	if (!rising) return;
	int index = port == AT32_GPIO_C && pin == 8 ? 0 : port == AT32_GPIO_F && pin == 6 ? 1 : -1;
	if (index < 0) return;
	chip_t *c = &chips[index];
	if (c->cs) return;
	if (c->driving) {
		c->shift = (c->shift << 1) | pins[2][index == 0 ? 6 : 9];
		c->bits++;
	} else {
		if (!c->reading) {
			assert(c->bits == 8 && (c->shift & 0x80));
			c->reading = true;
			unsigned reg = c->shift & 0x7F;
			c->response = reg == 0 ? c->id : c->regs[reg];
		}
		assert(++c->read_bits <= 16);
	}
}
void at32_pin_output(uint32_t port, unsigned pin, bool high)
{
	at32_pin_write(port, pin, high);
	if (port == AT32_GPIO_C && pin == 6) { chips[0].driving = true; chips[0].output_setups++; }
	if (port == AT32_GPIO_C && pin == 9) { chips[1].driving = true; chips[1].output_setups++; }
}
void at32_pin_input_floating(uint32_t port, unsigned pin)
{
	assert(port == AT32_GPIO_C && (pin == 6 || pin == 9));
	chips[pin == 6 ? 0 : 1].driving = false;
}
bool at32_pin_read(uint32_t port, unsigned pin)
{
	if ((port == AT32_GPIO_C && pin == 4) || (port == AT32_GPIO_F && pin == 8))
		return pins[(port - AT32_GPIO_A) / 0x400][pin];
	assert(port == AT32_GPIO_C && (pin == 6 || pin == 9));
	chip_t *c = &chips[pin == 6 ? 0 : 1];
	assert(c->reading && c->read_bits >= 1 && c->read_bits <= 16);
	return (c->response & (1u << (16 - c->read_bits))) != 0;
}
uint32_t at32_irq_save(void) { return critical_depth++; }
void at32_irq_restore(uint32_t saved) { assert(critical_depth == saved + 1); critical_depth = saved; }
void hal_delay_us(uint32_t us) { (void)us; assert(!panic); }
void hal_delay_ms(uint32_t ms)
{
	(void)ms;
	assert(!panic);
}

static void tx_is_blocked(void)
{
	unsigned before = gpio_writes;
	for (unsigned i = 0; i <= 255; i++) {
		assert(!rt880_radio.tx(145500000u, RADIO_BW_25K, (uint8_t)i, NULL));
		assert(!rt880_radio.tx(433500000u, RADIO_BW_12K5, (uint8_t)i, NULL));
	}
	assert(gpio_writes == before);
}
int main(void)
{
	assert(at32_pc13_needs_reset(1u << 27));
	assert(!at32_pc13_needs_reset((1u << 27) | (1u << 28)));
	assert(!at32_pc13_needs_reset(1u << 28));
	assert(!at32_pc13_needs_reset(0));
	chips[0].cs = chips[1].cs = true;
	chips[0].id = chips[1].id = BK4819_ID;
	tx_is_blocked();
	rt880_radio.init();
	// Receiver is the chip on bus_b (PF6/PA8/PC9) = chips[1]; bus_a = idle control.
	assert(chips[1].regs[0x30] == BK4819_STATE_IDLE_MAIN);
	assert(chips[0].regs[0x30] == 0 && chips[0].regs[0x33] == 0);
	assert(rt880_radio.rx(145500000, RADIO_BW_25K, NULL));
	assert(chips[1].regs[0x33] == 0x10 && chips[1].regs[0x30] == BK4819_STATE_RX);
	assert((((uint32_t)chips[1].regs[0x39] << 16) | chips[1].regs[0x38]) == 14550000);
	rt880_radio.audio(true);
	assert(!pins[2][4] && pins[5][8] && chips[1].regs[0x47] == BK4819_AF_FM);
	assert(chips[1].regs[0x48] == 0xB3B8);
	assert(rt880_radio.volume != NULL);
	rt880_radio.volume(0);
	assert(pins[2][4] && !pins[5][8] && chips[1].regs[0x47] == BK4819_AF_MUTE);
	rt880_radio.volume(100);
	assert(!pins[2][4] && pins[5][8] && chips[1].regs[0x48] == 0xB3BF);
	rt880_radio.volume(255);
	assert(chips[1].regs[0x48] == 0xB3BF);
	rt880_radio.audio(false);
	rt880_radio.volume(50);
	assert(pins[2][4] && !pins[5][8] && chips[1].regs[0x48] == 0xB3B8);
	rt880_radio.audio(true);
	char diag[48];
	assert(rt880_rf_diag(3, diag, sizeof(diag)) && strstr(diag, "AF:6140 G:B3B8"));
	assert(rt880_rf_diag(4, diag, sizeof(diag)) && strstr(diag, "Audio OPEN V:50"));
	chips[1].regs[0x67] = 140; chips[1].regs[0x65] = 23;
	radio_status_t status;
	unsigned setups = chips[1].output_setups;
	rt880_radio.status(&status);
	// Three reads: change direction once per command, never for each data bit.
	assert(chips[1].output_setups - setups <= 3);
	assert(status.rssi_dbm == -90 && status.noise == 23 && status.tone_ok);
	assert(rt880_radio.rx(433500000, RADIO_BW_12K5, NULL));
	assert(chips[1].regs[0x33] == 0x20 && pins[2][4] && !pins[5][8]);
	const uint32_t edges[] = { 136000000, 174000000, 400000000, 480000000 };
	for (unsigned i = 0; i < 4; i++) {
		assert(rt880_radio.rx(edges[i], RADIO_BW_25K, NULL));
		assert(chips[1].regs[0x33] == (i < 2 ? 0x10 : 0x20));
	}
	unsigned before = gpio_writes;
	for (uint32_t hz = 1000000; hz <= 1000000000; hz += 1000000) {
		if ((hz < 136000000 || hz > 174000000) && (hz < 400000000 || hz > 480000000))
			assert(!rt880_radio.rx(hz, RADIO_BW_25K, NULL));
	}
	subtone_t tone = { .type = TONE_CTCSS, .value = 885 };
	assert(!rt880_radio.rx(145500000, RADIO_BW_25K, &tone));
	assert(gpio_writes == before);
	tx_is_blocked();
	rt880_radio.set_mode(RADIO_MODE_DMR);
	assert(!rt880_radio.rx(145500000, RADIO_BW_25K, NULL));
	rt880_radio.set_mode(RADIO_MODE_FM);
	assert(rt880_radio.rx(145500000, RADIO_BW_25K, NULL));
	panic = true;
	rt880_rf_halt();
	panic = false;
	assert(chips[0].regs[0x30] == 0 && chips[1].regs[0x30] == 0);
	assert(chips[0].regs[0x33] == 0 && chips[1].regs[0x33] == 0);
	assert(pins[2][4] && !pins[5][8] && !pins[2][15] && !pins[5][0]);
	// A failed probe never starts initialization or permits tuning/audio.
	for (unsigned index = 0; index < 2; index++) {
		chips[index].id = 0xFFFF;
		unsigned writes_a = chips[0].writes, writes_b = chips[1].writes;
		rt880_radio.init();
		assert(chips[0].writes - writes_a <= 3 && chips[1].writes - writes_b <= 3);
		before = gpio_writes;
		assert(!rt880_radio.rx(145500000, RADIO_BW_25K, NULL));
		assert(gpio_writes == before);
		rt880_radio.audio(true);
		assert(pins[2][4] && !pins[5][8]);
		chips[index].id = BK4819_ID;
	}
	rt880_radio.init();
	assert(rt880_radio.rx(145500000, RADIO_BW_25K, NULL));
	assert(chips[1].regs[0x47] == BK4819_AF_MUTE && pins[2][4] && !pins[5][8]);
	assert(chips[1].regs[0x48] == 0xB3B8);
	assert(rt880_radio.rx(433500000, RADIO_BW_25K, NULL));
	rt880_radio.audio(true);
	assert(!pins[2][4] && pins[5][8] && chips[1].regs[0x47] == BK4819_AF_FM);
	assert(critical_depth == 0);
	printf("radio-test: RX routes/tuning/status/audio, ID failures and fault shutdown passed (%u frames); TX always denied\n", frames);
	return 0;
}
