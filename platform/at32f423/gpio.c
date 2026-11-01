// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// AT32F423 reference manual: CRM AHBEN1 and GPIO configuration/data registers.
#include "platform.h"
#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))

static uint32_t lock(void)
{
	uint32_t state;
	__asm volatile("mrs %0, primask\ncpsid i" : "=r"(state) :: "memory");
	return state;
}
static void unlock(uint32_t state)
{
	__asm volatile("msr primask, %0" :: "r"(state) : "memory");
}
static void clock_on(uint32_t port)
{
	REG(0x40023830u) |= 1u << ((port - AT32_GPIO_A) / 0x400u);
	(void)REG(0x40023830u);
}
void at32_pin_write(uint32_t port, unsigned pin, bool level)
{
	REG(port + 0x18u) = 1u << (pin + (level ? 0 : 16));
}
void at32_pin_output(uint32_t port, unsigned pin, bool level)
{
	uint32_t state = lock();
	clock_on(port);
	at32_pin_write(port, pin, level);
	REG(port + 0x04u) &= ~(1u << pin); // push-pull
	REG(port + 0x08u) &= ~(3u << (pin * 2)); // lowest drive strength
	REG(port + 0x0Cu) &= ~(3u << (pin * 2)); // no pull
	REG(port) = (REG(port) & ~(3u << (pin * 2))) | (1u << (pin * 2));
	unlock(state);
}
void at32_pin_input(uint32_t port, unsigned pin)
{
	uint32_t state = lock();
	clock_on(port);
	REG(port) &= ~(3u << (pin * 2));
	REG(port + 0x0Cu) = (REG(port + 0x0Cu) & ~(3u << (pin * 2))) | (2u << (pin * 2));
	unlock(state);
}
bool at32_pin_read(uint32_t port, unsigned pin)
{
	return (REG(port + 0x10u) & (1u << pin)) != 0;
}
