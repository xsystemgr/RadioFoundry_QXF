// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// UV380 family pin map (docs/spec/hardware-dm1701.md section 2), the same on every radio of the family but the MD-UV390
// Plus 10 W, which swaps PC6 and PD8 (hardware-mduv380.md 3: the mduv380 board's POWER=10w build), and the MD-2017
// (UVF_MD2017, boards/md2017: two AT1846S on PB6 / PB7, its own RX supplies, receiver audio switches, C6000 control
// bus and backlight; hardware-md2017.md 2, 3). Each board's own pins.h includes this one
#ifndef UVF_PINS_H
#define UVF_PINS_H

#include "platform.h"

#ifndef UVF_MD2017
#define UVF_MD2017		0	// 1: the MD-2017's pins (boards/md2017/board.mk)
#endif

// Analog
#define PIN_VOLUME		GPIOA, GPIO_PIN_0	// ADC1_IN0
#define PIN_BATTERY		GPIOA, GPIO_PIN_1	// ADC1_IN1
#define PIN_MIC_LEVEL		GPIOA, GPIO_PIN_3	// ADC1_IN3
#define PIN_APC_DAC		GPIOA, GPIO_PIN_4	// DAC_OUT1, PA power reference

// RF path
#if UVF_MD2017
// Receiver A (the AT1846S at 0x2E, the transceiver) and receiver B (0x71, receive only) each have their own UHF and
// VHF supplies (hardware-md2017.md 2.2); this firmware runs receiver A alone and keeps B's supplies off
#define PIN_RX_UHF_5V		GPIOC, GPIO_PIN_9
#define PIN_RX_VHF_5V		GPIOA, GPIO_PIN_5
#define PIN_RXB_UHF_5V		GPIOA, GPIO_PIN_8
#define PIN_RXB_VHF_5V		GPIOA, GPIO_PIN_2
#else
#define PIN_RX_UHF_5V		GPIOA, GPIO_PIN_2
#define PIN_RX_VHF_5V		GPIOA, GPIO_PIN_5
#endif
#define PIN_PA_EN1		GPIOC, GPIO_PIN_5	// TX supply
#define PIN_PA_EN2		GPIOC, GPIO_PIN_4	// APC loop enable
#if defined(RFOUNDRY_POWER_10W) && RFOUNDRY_POWER_10W
#define PIN_PA_BAND		GPIOD, GPIO_PIN_8	// low VHF, high UHF (10 W radio)
#else
#define PIN_PA_BAND		GPIOC, GPIO_PIN_6	// low VHF, high UHF
#endif

// Power, audio, LEDs
#define PIN_POWER_LATCH		GPIOA, GPIO_PIN_7
#if !UVF_MD2017	// the MD-2017 reads PA13 as an input and never drives PA9 / PA10 (hardware-md2017.md 3): not driven
#define PIN_MIC_POWER		GPIOA, GPIO_PIN_13	// SWDIO: driving it needs connect-under-reset for SWD
#define PIN_GPS_POWER		GPIOA, GPIO_PIN_9	// high = on (hardware-mduv380.md 9, hardware-dm1701.md 14.2)
#define PIN_GPS_RX		GPIOA, GPIO_PIN_10	// USART1 RX (AF7), the module's NMEA output
#endif
#define PIN_SPK_MUTE		GPIOB, GPIO_PIN_8	// high = muted
#define PIN_AMP_EN		GPIOB, GPIO_PIN_9	// high = on
#define PIN_RX_AUDIO_MUX	GPIOD, GPIO_PIN_9	// high = HR_C6000 line out to the amp (MD-2017: receiver A's audio on)
#if UVF_MD2017
#define PIN_RXB_AUDIO		GPIOD, GPIO_PIN_8	// receiver B's audio on the same line (hardware-md2017.md 2.3): kept low
#endif
#define PIN_LED_GREEN		GPIOE, GPIO_PIN_0
#define PIN_LED_RED		GPIOE, GPIO_PIN_1
#define PIN_CTCSS_PWM		GPIOC, GPIO_PIN_7	// unused, idles high (open drain)
#define PIN_BEEP_PWM		GPIOC, GPIO_PIN_8	// unused, idles low

#if UVF_MD2017
// The two AT1846S on PB6 / PB7, I2C1's pins (the TYT image bit-bangs them; hardware-md2017.md 2): receiver A at 0x2E
#define PIN_RF_SCL		GPIOB, GPIO_PIN_6	// AF4 open drain
#define PIN_RF_SDA		GPIOB, GPIO_PIN_7	// AF4 open drain
#define UVF_RF_I2C		I2C1
#define UVF_RF_I2C_AF		GPIO_AF4_I2C1
#define UVF_RF_I2C_CLK_ENABLE()	__HAL_RCC_I2C1_CLK_ENABLE()
#define AT1846S_B_I2C_ADDR	0x71	// receiver B
#else
// AT1846S on I2C3
#define PIN_RF_SCL		GPIOA, GPIO_PIN_8	// AF4 open drain
#define PIN_RF_SDA		GPIOC, GPIO_PIN_9	// AF4 open drain
#define UVF_RF_I2C		I2C3
#define UVF_RF_I2C_AF		GPIO_AF4_I2C3
#define UVF_RF_I2C_CLK_ENABLE()	__HAL_RCC_I2C3_CLK_ENABLE()
#endif
#define AT1846S_I2C_ADDR	0x2E

// HR_C6000
#define PIN_C6000_CS		GPIOE, GPIO_PIN_2
#if UVF_MD2017	// hardware-md2017.md 3.1
#define PIN_C6000_CLK		GPIOC, GPIO_PIN_13
#define PIN_C6000_MOSI		GPIOE, GPIO_PIN_5
#define PIN_C6000_MISO		GPIOE, GPIO_PIN_3
#else
#define PIN_C6000_CLK		GPIOE, GPIO_PIN_3
#define PIN_C6000_MOSI		GPIOE, GPIO_PIN_4
#define PIN_C6000_MISO		GPIOE, GPIO_PIN_5
#endif
#define PIN_C6000_PWD		GPIOE, GPIO_PIN_6	// high = powered down
#define PIN_C6000_VCS		GPIOB, GPIO_PIN_12	// voice SPI2 chip select
#define PIN_C6000_TS_INT	GPIOC, GPIO_PIN_0
#define PIN_C6000_SYS_INT	GPIOC, GPIO_PIN_1
#define PIN_C6000_RFTX_INT	GPIOC, GPIO_PIN_2

// SPI flash on SPI1 (AF5). The STM32F4 stub's board record reads these four (tools/boards.py F4_SPI_PINS)
#define PIN_FLASH_SCK		GPIOB, GPIO_PIN_3
#define PIN_FLASH_MISO		GPIOB, GPIO_PIN_4
#define PIN_FLASH_MOSI		GPIOB, GPIO_PIN_5
#define PIN_FLASH_CS		GPIOD, GPIO_PIN_7

// LCD on FSMC bank 1 (data lines double as keypad columns)
#define PIN_LCD_CS		GPIOD, GPIO_PIN_6
#define PIN_LCD_RST		GPIOD, GPIO_PIN_13
#if UVF_MD2017
#define PIN_LCD_BACKLIGHT	GPIOE, GPIO_PIN_4	// hardware-md2017.md 3.1
#elif defined(RFOUNDRY_POWER_10W) && RFOUNDRY_POWER_10W
#define PIN_LCD_BACKLIGHT	GPIOC, GPIO_PIN_6	// the 10 W radio
#else
#define PIN_LCD_BACKLIGHT	GPIOD, GPIO_PIN_8
#endif
#define LCD_CMD_ADDR		((volatile uint8_t *)0x60000000)
#define LCD_DATA_ADDR		((volatile uint8_t *)0x60040000)

// Keypad rows (driven high one at a time); the columns are the LCD data lines D0..D7 (platform.h STM32_FSMC_*)
#define PIN_KEY_ROW0		GPIOA, GPIO_PIN_6
#define PIN_KEY_ROW1		GPIOD, GPIO_PIN_2
#define PIN_KEY_ROW2		GPIOD, GPIO_PIN_3

// PTT and encoder
#define PIN_PTT			GPIOE, GPIO_PIN_11	// active low
#if !UVF_MD2017	// the MD-2017 reads PE12 as one of its keys, use unknown (hardware-md2017.md 3.1): not taken as PTT
#define PIN_PTT_EXT		GPIOE, GPIO_PIN_12	// active low
#endif
#define PIN_ENC_A		GPIOE, GPIO_PIN_14
#define PIN_ENC_B		GPIOB, GPIO_PIN_11	// EXTI11, both edges

// Helpers that take the two-argument pin macros above: PIN_WRITE(PIN_LED_RED, true)
#define PIN_PORT(...)		PIN_PORT2_(__VA_ARGS__)
#define PIN_MASK(...)		PIN_MASK2_(__VA_ARGS__)
#define PIN_PORT2_(port, pin)	(port)
#define PIN_MASK2_(port, pin)	(pin)
#define PIN_WRITE(p, level)	stm32_pin_write(PIN_PORT(p), PIN_MASK(p), (level))
#define PIN_READ(p)		stm32_pin_read(PIN_PORT(p), PIN_MASK(p))

#endif
