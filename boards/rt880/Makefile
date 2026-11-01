# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
# Experimental entry point, intentionally absent from the release/flasher registry.
# Run from the repository root: make -f boards/rt880/Makefile
.DEFAULT_GOAL := all
BUILD := build/rt880-lab
TARGET := $(BUILD)/rfoundry_rt880_rx_keys_fix4_UNTESTED
ARM_GCC_DIR ?= $(firstword $(wildcard $(HOME)/opt/arm-gnu-toolchain-*-arm-none-eabi/bin/) /usr/bin/)
CROSS ?= $(ARM_GCC_DIR)arm-none-eabi-
CC := $(CROSS)gcc
FEATURE_NAMES := $(shell python3 -c 'from pathlib import Path; print(Path("config/features.mk").read_text().split("FEATURES :=",1)[1].split("\n\n",1)[0].replace(chr(92)," "))')
$(foreach f,$(FEATURE_NAMES),$(eval CONFIG_$(f) := n))
$(foreach f,FM VFO VFO_FM DIAG UI_COLOR UI_SETTINGS SCAN KEY_ASSIGN CHANNEL_LIST,$(eval CONFIG_$(f) := y))
include config/features.mk
DIRS := core/app core/radio core/ui core/input core/codeplug core/settings core/storage \
	platform/at32f423 boards/rt880 drivers/display/st7789 drivers/rf/bk4819 drivers/rf/subaudio
SRC := $(foreach d,$(DIRS),$(wildcard $(d)/*.c))
SRC := $(filter-out core/app/task_stack.c core/app/watchdog.c,$(SRC))
RTOS := third_party/FreeRTOS-Kernel
SRC += $(addprefix $(RTOS)/,tasks.c queue.c list.c timers.c event_groups.c portable/GCC/ARM_CM4F/port.c portable/MemMang/heap_4.c)
OBJ := $(addprefix $(BUILD)/,$(SRC:.c=.o)) $(BUILD)/platform/at32f423/startup.o
CPU := -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard
DEFS := $(FEATURE_DEFS) -DGFX_INDEXED=1 -DGFX_FB_BYTES=9600u -DGFX_MAX_ROWS=160u \
	-DRFOUNDRY_BOARD_ID=\"rt880-lab\" -DRFOUNDRY_GIT=\"private-lab\"
INC := -Ihal -Icore -Idrivers -Iboards/rt880 -Iplatform/at32f423 -I$(BUILD)/gen -I$(RTOS)/include -I$(RTOS)/portable/GCC/ARM_CM4F
CFLAGS := $(CPU) -Os -g3 -std=gnu11 -ffunction-sections -fdata-sections -fno-common -flto=auto -fcallgraph-info=su -MMD -MP \
	-Wall -Wextra -Wshadow -Wno-unused-parameter $(DEFS) $(INC)
LDFLAGS := $(CPU) -Os -flto=auto -fcallgraph-info=su -Tboards/rt880/memory.ld -Wl,--gc-sections,-Map=$(TARGET).map,--print-memory-usage \
	--specs=nano.specs --specs=nosys.specs
all: $(TARGET).elf
$(TARGET).elf: $(OBJ) boards/rt880/memory.ld
	$(CC) $(LDFLAGS) $(OBJ) -lm -o $@
$(OBJ): boards/rt880/Makefile config/features.mk $(BUILD)/gen/rfoundry_about.h
$(BUILD)/gen/rfoundry_about.h: AUTHORS platform/at32f423/THIRD_PARTY tools/gen_about.py
	python3 tools/gen_about.py AUTHORS platform/at32f423/THIRD_PARTY $@
$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(CPU) -c $< -o $@
# Explicit opt-in produces a lab image, never a release or automatic flash action.
image: $(TARGET).elf
	$(CROSS)objcopy -O binary $< $(TARGET).bin
test:
	@mkdir -p $(BUILD)
	cc -std=c11 -Wall -Wextra -Werror -Ihal -Icore -Idrivers -Iplatform/at32f423 \
		boards/rt880/board.c boards/rt880/radio.c drivers/rf/bk4819/bk4819.c drivers/rf/subaudio/subaudio.c drivers/display/st7789/st7789.c core/input/input_engine.c boards/rt880/tests/board_test.c -o $(BUILD)/board-test
	$(BUILD)/board-test
	cc -std=c11 -Wall -Wextra -Werror -Ihal -Icore -Idrivers -Iplatform/at32f423 \
		boards/rt880/radio.c drivers/rf/bk4819/bk4819.c drivers/rf/subaudio/subaudio.c boards/rt880/tests/radio_test.c -o $(BUILD)/radio-test
	$(BUILD)/radio-test
stack-check: $(TARGET).elf
	python3 tools/stack_check.py --build $(BUILD) --label rt880-lab --cc '$(CC)' \
		--cflags '$(CFLAGS)' --elf $(TARGET).elf --overhead 208
-include $(OBJ:.o=.d)
.PHONY: all image test stack-check
