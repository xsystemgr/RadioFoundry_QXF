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
RT880_FEATURES := FM VFO VFO_FM DIAG UI_COLOR UI_SETTINGS SCAN CHANNEL_LIST AM_RX BCAST_FM FONT_AA DUAL_WATCH HOSTLINK CPS_OPENFW
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
RT880_FEATURES := FM VFO VFO_FM DIAG UI_COLOR UI_SETTINGS SCAN CHANNEL_LIST AM_RX BCAST_FM DUAL_WATCH HOSTLINK CPS_OPENFW
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
# The host link + CPS (codeplug programming) over the USART6 accessory serial (boards/rt880/serial_rt880.c). The host
# link adds a task, whose stack comes from the RTOS heap; the band-rendered framebuffer freed the RAM to grow the heap
# so it fits (12 KB is full with the base tasks). Only the host-link builds pay this - the default image stays at 12 KB.
ifeq ($(CONFIG_HOSTLINK),y)
DIRS += core/hostlink core/cps
HOSTLINK_DEFS := -DconfigTOTAL_HEAP_SIZE=15360
endif
# The build tag shown on the Home status strip (RFOUNDRY_GIT) so the operator can tell which image is flashed. Bump it
# each lab build; a diagnostic variant below overrides it with its own name. Override on the command line: RT880_VER=v7.
RT880_VER ?= v5-mic
# The GOGUFW-compatible analog messenger (BK4819 FSK) is a default analog feature now: the "Messages" hub entry opens
# it. Band-rendering freed the RAM for it to ride along in the normal Lite build. Disable with MSG=0; MSG=1 only renames
# the image so an explicit messenger build is easy to tell apart.
ifneq ($(MSG),0)
DIRS += core/messenger
endif
ifeq ($(MSG),1)
TARGET := $(BUILD)/rfoundry_rt880_MSG_UNTESTED
endif
# MIC_PROBE=1: a no-RF bench diagnostic. Runs the mic ADC on both BK4819 with the PA off and the antenna unrouted, so
# Diagnostics line 8 "mA:xxxx mB:xxxx" tracks the microphone amplitude per chip - the number that moves when you speak
# is the chip the mic is wired to (neither moving means the mic is not on a BK4819 at all).
ifeq ($(MIC_PROBE),1)
HF_DEFS += -DRT880_MIC_PROBE
TARGET := $(BUILD)/rfoundry_rt880_MICPROBE_UNTESTED
RT880_VER := MIC-PROBE
endif
# TONE_TEST=1: a voice PTT emits a fixed 1 kHz tone from the BK4819 tone generator instead of the mic, to isolate
# whether the modulator + RF path works (tone heard) or not (silent) - independent of the microphone routing.
ifeq ($(TONE_TEST),1)
HF_DEFS += -DRT880_TX_TONE_TEST
TARGET := $(BUILD)/rfoundry_rt880_TONETEST_UNTESTED
RT880_VER := TONE-TEST
endif
# TONE_MIC=1: decisive diagnostic - a 1 kHz tone is injected WITH the mic ADC bit left on, to tell whether enabling the
# mic mutes the modulator (tone vanishes) or not (tone heard = pure mic-source routing fault).
ifeq ($(TONE_MIC),1)
HF_DEFS += -DRT880_TONE_MIC
RT880_VER := TONE+MIC
endif
# Name every image after its on-screen version (RT880_VER), so the downloaded .bin can never be confused with another
# build - the file name matches exactly what the radio shows on its Home screen. '+' is swapped for '-' for the path.
TARGET := $(BUILD)/rfoundry_rt880_$(subst +,-,$(RT880_VER))_UNTESTED
SRC := $(foreach d,$(DIRS),$(wildcard $(d)/*.c))
SRC := $(filter-out core/app/task_stack.c core/app/watchdog.c,$(SRC))
RTOS := third_party/FreeRTOS-Kernel
SRC += $(addprefix $(RTOS)/,tasks.c queue.c list.c timers.c event_groups.c portable/GCC/ARM_CM4F/port.c portable/MemMang/heap_4.c)
OBJ := $(addprefix $(BUILD)/,$(SRC:.c=.o)) $(BUILD)/platform/at32f423/startup.o
CPU := -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard
DEFS := $(FEATURE_DEFS) $(HF_DEFS) $(HOSTLINK_DEFS) -DGFX_INDEXED=1 -DGFX_FB_BYTES=9600u -DGFX_MAX_ROWS=160u -DGFX_BAND_ROWS=20 \
	-DCONFIG_MESSENGER=$(if $(filter 0,$(MSG)),0,1) \
	-DRFOUNDRY_BOARD_ID=\"rt880-lab\" -DRFOUNDRY_GIT=\"$(RT880_VER)\" \
	-DCODEPLUG_CHANNELS_MAX=128 -DCODEPLUG_ADOPT_BLANK_ONLY=1 \
	$(if $(RT880_PA_BIAS),-DRT880_PA_BIAS=$(RT880_PA_BIAS),) $(if $(RT880_UART_SWEEP),-DRT880_UART_SWEEP,) $(if $(RT880_UART_ECHO),-DRT880_UART_ECHO,) \
	$(if $(LNA_FIND),-DRT880_LNA_FIND,)
INC := -Ihal -Icore -Idrivers -Iboards/rt880 -Iplatform/at32f423 -I$(BUILD)/gen -I$(RTOS)/include -I$(RTOS)/portable/GCC/ARM_CM4F
CFLAGS := $(CPU) -Os -g3 -std=gnu11 -ffunction-sections -fdata-sections -fno-common -flto=auto -fcallgraph-info=su -MMD -MP \
	-Wall -Wextra -Wshadow -Wno-unused-parameter $(DEFS) $(INC)
LDFLAGS := $(CPU) -Os -flto=auto -fcallgraph-info=su -Tboards/rt880/memory.ld -Wl,--gc-sections,-Map=$(TARGET).map,--print-memory-usage \
	--specs=nano.specs --specs=nosys.specs
all: $(TARGET).elf
$(TARGET).elf: $(OBJ) boards/rt880/memory.ld
	$(CC) $(LDFLAGS) $(OBJ) -lm -o $@
# RT880_VER is passed on the command line, so changing it does NOT touch any file make tracks - the objects that bake
# RFOUNDRY_GIT (the on-screen / splash / About version) would keep a stale version while the image file name changes.
# A stamp file updated only when RT880_VER changes forces exactly those objects to recompile, so the baked version
# always matches the file name and what the radio shows.
VERSTAMP := $(BUILD)/verstamp
$(shell mkdir -p $(BUILD); printf '%s' '$(RT880_VER)|$(RT880_PA_BIAS)' | cmp -s - $(VERSTAMP) 2>/dev/null || printf '%s' '$(RT880_VER)|$(RT880_PA_BIAS)' > $(VERSTAMP))
$(BUILD)/core/ui/ui_screens.o $(BUILD)/core/ui/ui.o $(BUILD)/core/ui/ui_menus.o $(BUILD)/boards/rt880/radio.o: $(VERSTAMP)
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
		boards/rt880/board.c boards/rt880/flash_rt880.c boards/rt880/radio.c drivers/rf/bk4819/bk4819.c drivers/rf/subaudio/subaudio.c drivers/display/st7789/st7789.c core/input/input_engine.c boards/rt880/tests/board_test.c -o $(BUILD)/board-test
	$(BUILD)/board-test
	cc -std=c11 -Wall -Wextra -Werror -DCONFIG_BCAST_FM=0 -Ihal -Icore -Idrivers -Iplatform/at32f423 \
		boards/rt880/radio.c drivers/rf/bk4819/bk4819.c drivers/rf/subaudio/subaudio.c boards/rt880/tests/radio_test.c -o $(BUILD)/radio-test
	$(BUILD)/radio-test
	# Band-rendering must reconstruct the same frame as the full-screen buffer: build gfx both ways and diff the dumps.
	cc -std=c11 -Wall -Wextra -Werror -DGFX_INDEXED=1 -DGFX_FB_BYTES=9600u -DGFX_MAX_ROWS=160u -DCONFIG_NET_WEBCTL=0 \
		-DCONFIG_FONT_AA=1 -DGFX_BAND_ROWS=0 -Ihal -Icore -Icore/ui core/ui/gfx.c boards/rt880/tests/gfx_band_test.c -o $(BUILD)/gfx-full
	cc -std=c11 -Wall -Wextra -Werror -DGFX_INDEXED=1 -DGFX_FB_BYTES=9600u -DGFX_MAX_ROWS=160u -DCONFIG_NET_WEBCTL=0 \
		-DCONFIG_FONT_AA=1 -DGFX_BAND_ROWS=20 -Ihal -Icore -Icore/ui core/ui/gfx.c boards/rt880/tests/gfx_band_test.c -o $(BUILD)/gfx-band
	$(BUILD)/gfx-full > $(BUILD)/gfx-full.txt
	$(BUILD)/gfx-band > $(BUILD)/gfx-band.txt
	cmp $(BUILD)/gfx-full.txt $(BUILD)/gfx-band.txt && echo "gfx band-render test: banded output matches full-screen"
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
