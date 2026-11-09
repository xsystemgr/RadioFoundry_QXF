// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// BK4819 FSK modem primitives for the GOGUFW-compatible analog messenger (1.2 kbaud, 100-byte frames). The register
// sequences match the GOGUFW UV-K1/K5 firmware so the frames interoperate over the air. The caller packs/unpacks the
// 50-word payload (core/messenger/msg_fsk.h); this layer only drives the chip.
#ifndef BK4819_FSK_H
#define BK4819_FSK_H

#include "bk4819.h"

// Configure the FSK modem (Tone2 1200 baud, FSK enable, CRC, 100-byte data length). Call before TX or RX arm.
void bk4819_fsk_setup(bk4819_t *dev);

// Transmit n words from the FIFO with the long (16-byte) preamble GOGUFW uses, and wait for TX-finished.
// Returns false on timeout. The chip must already be keyed into TX by the board.
bool bk4819_fsk_tx(bk4819_t *dev, const uint16_t *words, unsigned n);

// Arm FSK receive (clears the FIFO, enables RX with scramble, sets the RX IRQ mask). The chip must be in RX.
void bk4819_fsk_rx_arm(bk4819_t *dev);

// True once a full FSK frame has been received (REG_02 FSK_RX_FINISHED).
bool bk4819_fsk_rx_finished(bk4819_t *dev);

// Read n words out of the FSK RX FIFO (call after bk4819_fsk_rx_finished()).
void bk4819_fsk_rx_read(bk4819_t *dev, uint16_t *words, unsigned n);

// Incremental FIFO drain - the way the chip actually has to be serviced during RX. The 8-byte FSK FIFO overflows long
// before a 100-byte frame finishes, so the caller must poll this often (every radio status tick is enough at 1200 baud)
// instead of reading the whole frame at the end. It reads REG_02, resets *count on a fresh sync, drains 4 words per
// FIFO-almost-full, and on RX-finished drains the remainder up to max; it clears the handled IRQ flags. Returns true
// exactly once per frame, when RX-finished fired: *count then holds how many words were captured (<= max). Mirrors the
// GOGUFW interrupt handler (MSG_RF_OnRadioInterrupt).
bool bk4819_fsk_rx_service(bk4819_t *dev, uint16_t *words, unsigned *count, unsigned max);

// Return the modem to idle (interrupts off, standard preamble/sync).
void bk4819_fsk_reset(bk4819_t *dev);

#endif
