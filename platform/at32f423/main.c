// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// HICK 48 MHz receive/UI experiment; no PLL configuration or flash programming.
#include "platform.h"
#include "reset_policy.h"
#include "board.h"
#include "hal_time.h"
#include "FreeRTOS.h"
#include "task.h"
#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))

uint32_t SystemCoreClock = 8000000u;
uint8_t ucHeap[configTOTAL_HEAP_SIZE] __attribute__((aligned(8)));
static uint32_t previous_cycles;
static uint64_t elapsed_cycles;
extern uint32_t _vectors;
void app_start(void);
void xPortSysTickHandler(void);

void Default_Handler(void)
{
	__asm volatile("cpsid i" ::: "memory");
	board_rf_safe();
	for (;;) { __asm volatile("wfi"); }
}
void vApplicationMallocFailedHook(void) { Default_Handler(); }
void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
	(void)task; (void)name; Default_Handler();
}

static bool wait_bits(uint32_t addr, uint32_t mask, uint32_t value)
{
	for (uint32_t n = 0; n < 2000000u; n++) {
		if ((REG(addr) & mask) == value) return true;
	}
	return false;
}
static void setup(void)
{
	REG(0xE000E010u) = 0; // disable inherited SysTick
	for (unsigned n = 0; n < 8; n++) {
		REG(0xE000E180u + 4 * n) = 0xFFFFFFFFu;
		REG(0xE000E280u + 4 * n) = 0xFFFFFFFFu;
	}
	REG(0xE000ED04u) = (1u << 25) | (1u << 27);
	REG(0xE000ED08u) = (uint32_t)&_vectors;
	REG(0xE000ED0Cu) = 0x05FA0000u | (REG(0xE000ED0Cu) & 0x0000F8FFu); // priority group 0
	REG(0x400238A4u) |= 3u << 4; // automatic clock stepping
	REG(0x40023800u) |= 1u;
	if (!wait_bits(0x40023800u, 2u, 2u)) Default_Handler();
	// RM 4.1.1 and 4.3.23/24: divide HICK before changing its selection.
	// First allow any inherited supported clock; only performance bits change.
	REG(0x40023C00u) = (REG(0x40023C00u) & ~7u) | 4u;
	REG(0x400238A4u) = (REG(0x400238A4u) & ~(7u << 16)) | (4u << 16);
	for (unsigned n = 0; n < 64; n++) __asm volatile("nop");
	REG(0x40023808u) &= ~3u;
	if (!wait_bits(0x40023808u, 12u, 0)) Default_Handler();
	// HICKDIV must not change while a HICK-fed PLL is enabled.
	REG(0x40023800u) &= ~(1u << 24);
	if (!wait_bits(0x40023800u, 1u << 25, 0)) Default_Handler();
	REG(0x400238A0u) |= 1u << 14; // HICK_TO_SCLK (register table: MISC1)
	REG(0x400238A0u) |= 1u << 12; // HICKDIV = undivided 48 MHz
	REG(0x40023808u) &= ~0xFCF0u; // AHB/APB1/APB2 = 1
	REG(0x40023C00u) = (REG(0x40023C00u) & ~7u) | 1u; // 32..64 MHz
	REG(0x400238A4u) &= ~(7u << 16); // HICK/16 -> HICK
	__asm volatile("dsb\nisb" ::: "memory");
	SystemCoreClock = AT32_CORE_HZ;
	REG(0xE000EDFCu) |= 1u << 24; // enable DWT cycle counter
	REG(0xE0001004u) = 0;
	REG(0xE0001000u) |= 1u;
}
static uint64_t cycles(void)
{
	uint32_t state;
	__asm volatile("mrs %0, primask\ncpsid i" : "=r"(state) :: "memory");
	uint32_t now = REG(0xE0001004u);
	elapsed_cycles += (uint32_t)(now - previous_cycles);
	previous_cycles = now;
	uint64_t result = elapsed_cycles;
	__asm volatile("msr primask, %0" :: "r"(state) : "memory");
	return result;
}
uint32_t hal_micros(void) { return (uint32_t)(cycles() / (AT32_CORE_HZ / 1000000u)); }
uint32_t hal_millis(void) { return (uint32_t)(cycles() / (AT32_CORE_HZ / 1000u)); }
void hal_delay_us(uint32_t us)
{
	while (us) {
		uint32_t chunk = us > 1000u ? 1000u : us;
		uint32_t start = REG(0xE0001004u);
		while ((uint32_t)(REG(0xE0001004u) - start) < chunk * (AT32_CORE_HZ / 1000000u)) {}
		us -= chunk;
	}
}
void hal_delay_ms(uint32_t ms)
{
	if (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) {
		vTaskDelay(pdMS_TO_TICKS(ms ? ms : 1));
	} else {
		while (ms--) hal_delay_us(1000);
	}
}
void vApplicationTickHook(void) { (void)cycles(); }
void SysTick_Handler(void) { xPortSysTickHandler(); }
int main(void)
{
	if (at32_pc13_needs_reset(REG(0x40023874u))) {
		board_rf_safe();
		__asm volatile("dsb" ::: "memory");
		REG(0xE000ED0Cu) = 0x05FA0004u | (REG(0xE000ED0Cu) & 0x700u);
		__asm volatile("dsb" ::: "memory");
		for (;;) {}
	}
	setup();
	board_rf_safe();
	board_early_init();
	app_start();
	vTaskStartScheduler();
	Default_Handler();
	return 0;
}
