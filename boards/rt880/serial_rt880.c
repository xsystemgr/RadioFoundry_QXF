// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// RT-880 host-link serial port: USART6 on PA11 (TX) / PA12 (RX). Confirmed on hardware: the radio's USB-C port drives
// an on-board CP210x USB-serial bridge whose UART side lands on PA11/PA12, at 115200 8N1 - a banner sent on PA11 reaches
// the PC, and bytes sent from the PC echo back from PA12. So the PC's existing USB-C cable reaches this port; no
// accessory FPC or adapter is needed. This is the byte stream core/hostlink drives for the CPS (codeplug programming).
//
// Register facts (AT32F423, confirmed against the Artery firmware library / reference manual, NOT the STM32 layout):
//   USART6 base 0x40011400 (on APB2): STS 0x00 (RDBF bit5 = byte received, TDC bit6 = transmit done, TDBE bit7 = can
//                           write), DT 0x04, BAUDR 0x08 (= APB2 clock / baud, rounded), CTRL1 0x0C (REN bit2, TEN bit3,
//                           RDBFIEN bit5, UEN bit13). APB2 clock = HCLK = 48 MHz.
//   Clock: CRM APB2EN 0x40023844 bit 5. GPIOA clock: CRM AHBEN1 0x40023830 bit 0.
//   GPIOA 0x40020000: CFGR 0x00 (2 bits/pin, 0b10 = alternate), PULL 0x0C, MUXH 0x24 (4 bits/pin, pins 8..15).
//   PA11/PA12 take alternate function MUX8 = USART6. IRQ: USART6_IRQn = 71 (slot in startup.S).
#include "board.h"
#include "platform.h"
#include "hal_stream.h"
#include "hal_time.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define REGW(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define REGB(a) (*(volatile uint8_t  *)(uintptr_t)(a))
#define USART6		0x40011400u
#define U_STS		0x00u
#define U_DT		0x04u
#define U_BAUDR		0x08u
#define U_CTRL1		0x0Cu
#define GPIOA		0x40020000u
#define CRM_APB1EN	0x40023840u	// (USART3 = bit 18, for the diagnostic sweep)
#define CRM_APB2EN	0x40023844u	// USART6 = bit 5 (USART1 = bit 4)
#define CRM_AHBEN1	0x40023830u	// GPIOAEN = bit 0, GPIOB = bit 1, GPIOC = bit 2
#define USART6_IRQ	71u
#define PIN_TX		11u		// PA11
#define PIN_RX		12u		// PA12
#define NVIC_ISER	0xE000E100u
#define NVIC_IPR	0xE000E400u

#ifndef RT880_UART_BAUD
#define RT880_UART_BAUD 115200u
#endif
#define UART_CLK 48000000u

// RX ring filled by the interrupt, drained by the host-link task. Power-of-two size so the mask is a simple &.
#define RX_SZ 256u
static volatile uint8_t rx_ring[RX_SZ];
static volatile uint16_t rx_head, rx_tail;	// head: ISR writes; tail: reader
static bool started;

// Drain whatever bytes the USART has into the ring. Called from the ISR and, as a safety net, from the reader - so a
// receive works even if the interrupt never reaches us.
static void pump(void)
{
	while (REGW(USART6 + U_STS) & (1u << 5)) {		// RDBF: a byte is waiting
		uint8_t b = (uint8_t)REGW(USART6 + U_DT);	// reading DT clears RDBF
		uint16_t next = (uint16_t)((rx_head + 1u) & (RX_SZ - 1u));
		if (next != rx_tail) {				// drop on overflow rather than overwrite unread bytes
			rx_ring[rx_head] = b;
			rx_head = next;
		}
	}
}

// __attribute__((used)) is essential: the handler is referenced only from the assembly vector table (startup.S), which
// link-time optimisation does not analyse, so without this LTO drops it as "unused" and the weak fallback aliases the
// vector to Default_Handler - the receive interrupt then does nothing. (That was the bug that made the CPS silent.)
__attribute__((used)) void USART6_IRQHandler(void)
{
	pump();
}

#ifdef RT880_UART_SWEEP
static void rt880_uart_sweep(void);
#endif
#ifdef RT880_UART_ECHO
static void rt880_uart_echo(void);
#endif

static bool uart_start(void)
{
	if (started)
		return true;
	REGW(CRM_AHBEN1) |= 1u << 0;		// GPIOA clock (already on in practice; harmless)
	REGW(CRM_APB2EN) |= 1u << 5;		// USART6 clock
	// PA11 (TX) and PA12 (RX) to alternate function MUX8 (pins 8..15 -> MUXH, 4 bits each).
	REGW(GPIOA + 0x00u) = (REGW(GPIOA + 0x00u) & ~((3u << (PIN_TX * 2)) | (3u << (PIN_RX * 2))))
			    | (2u << (PIN_TX * 2)) | (2u << (PIN_RX * 2));				// CFGR = mux
	REGW(GPIOA + 0x24u) = (REGW(GPIOA + 0x24u) & ~((0xFu << ((PIN_TX - 8u) * 4u)) | (0xFu << ((PIN_RX - 8u) * 4u))))
			    | (8u << ((PIN_TX - 8u) * 4u)) | (8u << ((PIN_RX - 8u) * 4u));		// MUXH = 8
	REGW(GPIOA + 0x0Cu) = (REGW(GPIOA + 0x0Cu) & ~(3u << (PIN_RX * 2))) | (1u << (PIN_RX * 2));	// PA12 (RX) pull-up
	REGW(GPIOA + 0x08u) |= 2u << (PIN_TX * 2);	// PA11 (TX) a moderate drive strength
	// USART6: set the baud, enable receive + transmit + the receive interrupt, then enable the peripheral.
	REGW(USART6 + U_CTRL1) = 0;
	REGW(USART6 + U_BAUDR) = (UART_CLK + RT880_UART_BAUD / 2u) / RT880_UART_BAUD;
	REGW(USART6 + U_CTRL1) = (1u << 2) | (1u << 3) | (1u << 5);	// REN | TEN | RDBFIEN
	REGW(USART6 + U_CTRL1) |= 1u << 13;				// UEN
	// NVIC: a low priority (the handler only fills a ring, it calls nothing in the kernel), then enable IRQ 71.
	REGB(NVIC_IPR + USART6_IRQ) = 0xC0u;
	REGW(NVIC_ISER + (USART6_IRQ / 32u) * 4u) = 1u << (USART6_IRQ % 32u);
	started = true;
#ifdef RT880_UART_SWEEP
	rt880_uart_sweep();
#endif
#ifdef RT880_UART_ECHO
	rt880_uart_echo();
#endif
	return true;
}

static bool uart_connected(void) { return true; }	// a bare UART is always "there" (no DTR to sense)

static size_t uart_read(uint8_t *buf, size_t max)
{
	pump();						// safety net in case the interrupt is not reaching us
	size_t n = 0;
	while (n < max && rx_tail != rx_head) {
		buf[n++] = rx_ring[rx_tail];
		rx_tail = (uint16_t)((rx_tail + 1u) & (RX_SZ - 1u));
	}
	return n;
}

static bool uart_wait_rx(uint32_t timeout_ms)
{
	uint32_t start = hal_millis();
	while (rx_tail == rx_head) {
		pump();					// catch bytes even without the interrupt
		if (rx_tail != rx_head)
			return true;
		if (hal_millis() - start >= timeout_ms)
			return false;
		vTaskDelay(1);				// yield; the ISR and/or the next pump() will get the bytes
	}
	return true;
}

static size_t uart_write(const uint8_t *buf, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		uint32_t guard = 0;
		while (!(REGW(USART6 + U_STS) & (1u << 7)))	// TDBE: transmit buffer can take a byte
			if (++guard > 4000000u)
				return i;			// the port is wedged; report what went
		REGW(USART6 + U_DT) = buf[i];
	}
	uint32_t guard = 0;
	while (!(REGW(USART6 + U_STS) & (1u << 6)) && ++guard < 4000000u) {}	// let the last byte finish (TDC)
	return len;
}

const hal_stream_t rt880_host_stream = {
	.start = uart_start,
	.connected = uart_connected,
	.read = uart_read,
	.wait_rx = uart_wait_rx,
	.write = uart_write,
};

#ifdef RT880_UART_SWEEP
// Bring-up diagnostic: labelled banner on each candidate TX pin (which one reaches cps_tool.py --listen is the CP210x
// pin). Kept for future bring-up of other boards/cables.
static void diag_tx_one(uint32_t usart, uint32_t crm_reg, unsigned crm_bit, uint32_t gpio, unsigned pin, unsigned mux,
			const char *label)
{
	REGW(CRM_AHBEN1) |= 1u << ((gpio - 0x40020000u) / 0x400u);
	REGW(crm_reg) |= 1u << crm_bit;
	REGW(gpio + 0x00u) = (REGW(gpio + 0x00u) & ~(3u << (pin * 2))) | (2u << (pin * 2));
	uint32_t moff = pin < 8 ? 0x20u : 0x24u;
	unsigned msh = (pin % 8u) * 4u;
	REGW(gpio + moff) = (REGW(gpio + moff) & ~(0xFu << msh)) | ((uint32_t)mux << msh);
	REGW(usart + U_CTRL1) = 0;
	REGW(usart + U_BAUDR) = (UART_CLK + RT880_UART_BAUD / 2u) / RT880_UART_BAUD;
	REGW(usart + U_CTRL1) = (1u << 3) | (1u << 13);
	for (int rep = 0; rep < 6; rep++) {
		for (const char *p = label; *p; p++) {
			uint32_t g = 0;
			while (!(REGW(usart + U_STS) & (1u << 7)) && ++g < 1000000u) {}
			REGW(usart + U_DT) = (uint8_t)*p;
		}
		for (volatile uint32_t d = 0; d < 150000u; d++) {}
	}
}

static void rt880_uart_sweep(void)
{
	for (int loop = 0; loop < 6; loop++) {
		diag_tx_one(0x40011400u, CRM_APB2EN, 5u, 0x40020000u, 11u, 8u, "**U6_PA11**\r\n");
		diag_tx_one(0x40013800u, CRM_APB2EN, 4u, 0x40020000u,  9u, 7u, "**U1_PA9**\r\n");
		diag_tx_one(0x40004800u, CRM_APB1EN, 18u,0x40020400u, 10u, 7u, "**U3_PB10**\r\n");
		diag_tx_one(0x40004400u, CRM_APB1EN, 17u,0x40020000u,  2u, 7u, "**U2_PA2**\r\n");
		diag_tx_one(0x40004800u, CRM_APB1EN, 18u,0x40020800u, 10u, 7u, "**U3_PC10**\r\n");
	}
}
#endif

#ifdef RT880_UART_ECHO
// Bring-up diagnostic: polled echo on USART6 PA11/PA12 (confirms the receive pin without the interrupt).
static void rt880_uart_echo(void)
{
	const char *hello = "USART6 ECHO on PA11/PA12 - type something:\r\n";
	for (const char *p = hello; *p; p++) {
		uint32_t g = 0;
		while (!(REGW(USART6 + U_STS) & (1u << 7)) && ++g < 1000000u) {}
		REGW(USART6 + U_DT) = (uint8_t)*p;
	}
	for (;;) {
		if (REGW(USART6 + U_STS) & (1u << 5)) {
			uint8_t b = (uint8_t)REGW(USART6 + U_DT);
			char out[4] = { '[', (char)b, ']', 0 };
			for (char *p = out; *p; p++) {
				uint32_t g = 0;
				while (!(REGW(USART6 + U_STS) & (1u << 7)) && ++g < 1000000u) {}
				REGW(USART6 + U_DT) = (uint8_t)*p;
			}
		}
	}
}
#endif
