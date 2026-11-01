// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
#ifndef PLATFORM_AT32F423_H
#define PLATFORM_AT32F423_H
#include <stdint.h>
#include <stdbool.h>

#define AT32_CORE_HZ 48000000u

#define AT32_GPIO_A 0x40020000u
#define AT32_GPIO_B 0x40020400u
#define AT32_GPIO_C 0x40020800u
#define AT32_GPIO_D 0x40020C00u
#define AT32_GPIO_F 0x40021400u

void at32_pin_output(uint32_t port, unsigned pin, bool level);
void at32_pin_input(uint32_t port, unsigned pin);
void at32_pin_input_floating(uint32_t port, unsigned pin);
uint32_t at32_irq_save(void);
void at32_irq_restore(uint32_t state);
void at32_port_write(uint32_t port, uint32_t set_clear);
void at32_pin_write(uint32_t port, unsigned pin, bool level);
bool at32_pin_read(uint32_t port, unsigned pin);
extern uint32_t SystemCoreClock;
#endif
