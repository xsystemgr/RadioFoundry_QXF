// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// BK4819 FSK modem for the GOGUFW messenger (bk4819_fsk.h). Register values and order match the GOGUFW firmware.
#include <stddef.h>
#include "bk4819_fsk.h"

// FSK registers (not all are in bk4819.h; the messenger owns these)
#define FSK_REG_IRQ_FLAGS	0x02	// read: which interrupts fired (REG_02 flags)
#define FSK_REG_IRQ_PENDING	0x0C	// read: bit 0 = an interrupt is pending
#define FSK_REG_IRQ_MASK	0x3F	// write: which interrupts are enabled
#define FSK_REG_58		0x58	// FSK enable / bandwidth / mode
#define FSK_REG_59		0x59	// FIFO clear, preamble/sync length, RX/TX enable, scramble
#define FSK_REG_5C		0x5C	// CRC and framing
#define FSK_REG_5D		0x5D	// data length
#define FSK_REG_FIFO		0x5F	// FSK FIFO data port
#define FSK_REG_70		0x70	// Tone2 enable / gain
#define FSK_REG_72		0x72	// Tone2 baud rate

// REG_02 / REG_3F interrupt bits
#define FSK_IRQ_TX_FINISHED	(1u << 15)
#define FSK_IRQ_RX_FINISHED	(1u << 13)
#define FSK_IRQ_FIFO_ALMOST_FULL (1u << 12)
#define FSK_IRQ_RX_SYNC		(1u << 1)
#define FSK_RX_IRQ_MASK		(FSK_IRQ_RX_SYNC | FSK_IRQ_RX_FINISHED | FSK_IRQ_FIFO_ALMOST_FULL)

// REG_5D: 100-byte data length (the messenger frame; GOGUFW Aircopy uses 0x4700 / 72 bytes)
#define FSK_LEN_100_BYTES	0x6300
// REG_59 with the maximum 16-byte preamble (bits <7:4> = 0xF), for TX lead-in
#define FSK_REG59_TX_CLEAR	0x80F8
#define FSK_REG59_TX_READY	0x00F8
#define FSK_REG59_TX_START	0x28F8
// REG_59 for RX: 7-byte preamble, 4-byte sync; clear then enable (RX + scramble)
#define FSK_REG59_RX_CLEAR	0x4068
#define FSK_REG59_RX_ENABLE	0x3068
#define FSK_REG59_IDLE		0x0068

static void fsk_delay_ms(bk4819_t *dev, uint32_t ms)
{
	if (dev->bus->delay_us != NULL) {
		dev->bus->delay_us(ms * 1000u);
	}
}

void bk4819_fsk_setup(bk4819_t *dev)
{
	bk4819_write(dev, FSK_REG_70, 0x00E0);		// enable Tone2, tuning gain 48
	bk4819_write(dev, FSK_REG_72, 0x3065);		// Tone2 baud rate 1200
	bk4819_write(dev, FSK_REG_58, 0x00C1);		// FSK enable, 1.2k RX bandwidth, FSK 1.2k/2.4k TX/RX
	bk4819_write(dev, FSK_REG_5C, 0x5665);		// CRC and framing
	bk4819_write(dev, FSK_REG_5D, FSK_LEN_100_BYTES);
}

void bk4819_fsk_reset(bk4819_t *dev)
{
	bk4819_write(dev, FSK_REG_IRQ_MASK, 0x0000);
	bk4819_write(dev, FSK_REG_59, FSK_REG59_IDLE);
}

bool bk4819_fsk_tx(bk4819_t *dev, const uint16_t *words, unsigned n)
{
	fsk_delay_ms(dev, 30);
	bk4819_write(dev, FSK_REG_IRQ_MASK, FSK_IRQ_TX_FINISHED);
	bk4819_write(dev, FSK_REG_5D, FSK_LEN_100_BYTES);
	bk4819_write(dev, FSK_REG_59, FSK_REG59_TX_CLEAR);
	bk4819_write(dev, FSK_REG_59, FSK_REG59_TX_READY);
	for (unsigned i = 0; i < n; i++) {
		bk4819_write(dev, FSK_REG_FIFO, words[i]);
	}
	fsk_delay_ms(dev, 20);
	bk4819_write(dev, FSK_REG_59, FSK_REG59_TX_START);

	bool done = false;
	for (unsigned t = 0; t < 200u; t++) {		// up to ~1 s for the burst to finish
		if (bk4819_read(dev, FSK_REG_IRQ_PENDING) & 1u) {
			done = true;
			break;
		}
		fsk_delay_ms(dev, 5);
	}
	bk4819_write(dev, FSK_REG_IRQ_FLAGS, 0x0000);
	fsk_delay_ms(dev, 30);
	bk4819_fsk_reset(dev);
	return done;
}

void bk4819_fsk_rx_arm(bk4819_t *dev)
{
	bk4819_fsk_setup(dev);
	bk4819_write(dev, FSK_REG_5D, FSK_LEN_100_BYTES);
	bk4819_write(dev, FSK_REG_IRQ_FLAGS, 0x0000);
	bk4819_write(dev, FSK_REG_IRQ_MASK, FSK_RX_IRQ_MASK);
	bk4819_write(dev, FSK_REG_59, FSK_REG59_RX_CLEAR);
	bk4819_write(dev, FSK_REG_59, FSK_REG59_RX_ENABLE);
}

bool bk4819_fsk_rx_finished(bk4819_t *dev)
{
	return (bk4819_read(dev, FSK_REG_IRQ_FLAGS) & FSK_IRQ_RX_FINISHED) != 0;
}

void bk4819_fsk_rx_read(bk4819_t *dev, uint16_t *words, unsigned n)
{
	for (unsigned i = 0; i < n; i++) {
		words[i] = bk4819_read(dev, FSK_REG_FIFO);
	}
}

bool bk4819_fsk_rx_service(bk4819_t *dev, uint16_t *words, unsigned *count, unsigned max)
{
	bool finished = false;
	// BK4819 interrupt read protocol (exactly as the GOGUFW/UV-K5 main loop does it): the chip only presents the
	// interrupt status in REG_02 after REG_0C bit0 shows a request AND REG_02 is written to 0 to latch it. Reading
	// REG_02 directly (without this handshake) returns nothing, which is why an earlier version never saw a frame.
	// Drain every pending interrupt; the write-0 ack clears REG_0C so the loop terminates. Bounded for safety.
	for (unsigned guard = 0; guard < 64u && (bk4819_read(dev, FSK_REG_IRQ_PENDING) & 1u); guard++) {
		bk4819_write(dev, FSK_REG_IRQ_FLAGS, 0x0000);		// ack the request latch
		uint16_t status = bk4819_read(dev, FSK_REG_IRQ_FLAGS);	// now holds which interrupts fired
		// A fresh sync marks a new frame boundary: restart the capture at word zero (so a partial tail from a
		// previous burst never bleeds into the real packet).
		if (status & FSK_IRQ_RX_SYNC) {
			*count = 0;
		}
		// FIFO almost full: drain 4 words (8 bytes) now, before the hardware overruns it.
		if (status & FSK_IRQ_FIFO_ALMOST_FULL) {
			for (unsigned i = 0; i < 4u && *count < max; i++) {
				words[(*count)++] = bk4819_read(dev, FSK_REG_FIFO);
			}
		}
		if (status & FSK_IRQ_RX_FINISHED) {
			while (*count < max) {				// drain whatever is left of the frame
				words[(*count)++] = bk4819_read(dev, FSK_REG_FIFO);
			}
			finished = true;
		}
	}
	return finished;
}
