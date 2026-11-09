# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
# Experimental entry point, intentionally absent from the release/flasher registry.
# Run from the repository root: make -f boards/rt880/Makefile
.DEFAULT_GOAL := all
# HF=1 builds the Si4732 broadcast/HF image: the HF receiver appears as a second VFO (CONFIG_BCAST_FM), keeping the
# dual-watch so it shows as a real second VFO, but dropping the RAM-heavy DM-32 fonts so the broadcast stack fits.
# The default build keeps the DM-32 fonts and leaves the Si4732 out. The two configs use separate build directories
# so their objects (compiled with different CONFIG_BCAST_FM) never clobber each other.
ifeq ($(HF),1)
BUILD := build/rt880-lab-hf
RT880_FEATURES := FM VFO VFO_FM DIAG UI_COLOR UI_SETTINGS SCAN CHANNEL_LIST AM_RX BCAST_FM FONT_AA
TARGET := $(BUILD)/rfoundry_rt880_HF_si4732_UNTESTED
# The Si4732 keeps 0.15-24 MHz (AM/SW); 24-30 MHz (CB/10 m) is left to the BK4819 for narrowband FM (see route()).
# CONFIG_SI4732_SSB pulls in the SSB patch (ham voice on HF; third-party patch, personal use - see si4732_ssb_patch.h).
HF_DEFS := -DSI4732_HI_KHZ=24000u -DCONFIG_SI4732_SSB=1
# ANTPROBE=1: a bench diagnostic that cycles candidate GPIOs (HIGH/LOW) to find the HF antenna / LNA switch the
# factory drives but we don't - tune a strong MW/SW station and watch the HF RSSI (diag) jump on the right pin.
ifeq ($(ANTPROBE),1)
HF_DEFS += -DRT880_HF_ANTPROBE
TARGET := $(BUILD)/rfoundry_rt880_HF_ANTPROBE_UNTESTED
endif
else
BUILD := build/rt880-lab
RT880_FEATURES := FM VFO VFO_FM DIAG UI_COLOR UI_SETTINGS SCAN CHANNEL_LIST AM_RX BCAST_FM DUAL_WATCH
TARGET := $(BUILD)/rfoundry_rt880_rx_speaker_fix5_UNTESTED
HF_DEFS :=
endif
ARM_GCC_DIR ?= $(firstword $(wildcard $(HOME)/opt/arm-gnu-toolchain-*-arm-none-eabi/bin/) /usr/bin/)
CROSS ?= $(ARM_GCC_DIR)arm-none-eabi-
CC := $(CROSS)gcc
FEATURE_NAMES := $(shell python3 -c 'from pathlib import Path; print(Path("config/features.mk").read_text().split("FEATURES :=",1)[1].split("\n\n",1)[0].replace(chr(92)," "))')
$(foreach f,$(FEATURE_NAMES),$(eval CONFIG_$(f) := n))
$(foreach f,$(RT880_FEATURES),$(eval CONFIG_$(f) := y))
include config/features.mk
DIRS := core/app core/radio core/ui core/input core/codeplug core/settings core/storage \
	platform/at32f423 boards/rt880 drivers/display/st7789 drivers/rf/bk4819 drivers/rf/subaudio
# The Si4732 broadcast/HF driver and its bit-banged I2C transport come in whenever the broadcast VFO
# is enabled (CONFIG_BCAST_FM) - both the default speaker-fix image and the HF=1 image use it - so the
# core/board references to si4732_hal always have a definition to link against.
ifeq ($(CONFIG_BCAST_FM),y)
DIRS += drivers/bcast/si4732 drivers/bus/i2c_gpio
endif
# MSG=1: the GOGUFW-compatible analog messenger (BK4819 FSK). Build on the lean HF base: make HF=1 MSG=1 image
ifeq ($(MSG),1)
DIRS += core/messenger
TARGET := $(BUILD)/rfoundry_rt880_MSG_UNTESTED
endif
SRC := $(foreach d,$(DIRS),$(wildcard $(d)/*.c))
SRC := $(filter-out core/app/task_stack.c core/app/watchdog.c,$(SRC))
RTOS := third_party/FreeRTOS-Kernel
SRC += $(addprefix $(RTOS)/,tasks.c queue.c list.c timers.c event_groups.c portable/GCC/ARM_CM4F/port.c portable/MemMang/heap_4.c)
OBJ := $(addprefix $(BUILD)/,$(SRC:.c=.o)) $(BUILD)/platform/at32f423/startup.o
CPU := -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard
DEFS := $(FEATURE_DEFS) $(HF_DEFS) -DGFX_INDEXED=1 -DGFX_FB_BYTES=9600u -DGFX_MAX_ROWS=160u \
	-DCONFIG_MESSENGER=$(if $(filter 1,$(MSG)),1,0) \
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
	cc -std=c11 -Wall -Wextra -Werror -DCONFIG_BCAST_FM=0 -Ihal -Icore -Idrivers -Iplatform/at32f423 \
		boards/rt880/board.c boards/rt880/radio.c drivers/rf/bk4819/bk4819.c drivers/rf/subaudio/subaudio.c drivers/display/st7789/st7789.c core/input/input_engine.c boards/rt880/tests/board_test.c -o $(BUILD)/board-test
	$(BUILD)/board-test
	cc -std=c11 -Wall -Wextra -Werror -DCONFIG_BCAST_FM=0 -Ihal -Icore -Idrivers -Iplatform/at32f423 \
		boards/rt880/radio.c drivers/rf/bk4819/bk4819.c drivers/rf/subaudio/subaudio.c boards/rt880/tests/radio_test.c -o $(BUILD)/radio-test
	$(BUILD)/radio-test
# The Si4732 HF driver and its bit-banged I2C transport, host-side with a mock chip.
test-hf:
	@mkdir -p $(BUILD)
	cc -std=c11 -Wall -Wextra -Werror -Ihal -Icore -Idrivers \
		drivers/bcast/si4732/si4732.c boards/rt880/tests/si4732_test.c -o $(BUILD)/si4732-test
	$(BUILD)/si4732-test
# The GOGUFW-compatible GGM2 messenger packet (wire layout + CRC) and the in-RAM message store, host-side.
test-msg:
	@mkdir -p $(BUILD)
	cc -std=c11 -Wall -Wextra -Werror -Icore \
		core/messenger/msg_packet.c core/messenger/msg_fsk.c core/messenger/tests/msg_packet_test.c -o $(BUILD)/msg-packet-test
	$(BUILD)/msg-packet-test
	cc -std=c11 -Wall -Wextra -Werror -Icore \
		core/messenger/msg_store.c core/messenger/msg_packet.c core/messenger/tests/msg_store_test.c -o $(BUILD)/msg-store-test
	$(BUILD)/msg-store-test
stack-check: $(TARGET).elf
	python3 tools/stack_check.py --build $(BUILD) --label rt880-lab --cc '$(CC)' \
		--cflags '$(CFLAGS)' --elf $(TARGET).elf --overhead 208
-include $(OBJ:.o=.d)
.PHONY: all image test test-hf test-msg stack-check
