ZIG ?= zig
READELF ?= $(firstword $(wildcard /opt/homebrew/opt/llvm/bin/llvm-readelf \
	/usr/local/opt/llvm/bin/llvm-readelf /usr/bin/llvm-readelf \
	/usr/bin/readelf) llvm-readelf)
TARGET ?= arm-linux-gnueabihf.2.34
CPU ?= cortex_a7
BUILD_DIR ?= build
HOST_CC ?= cc
HOST_CXX ?= c++
PYTHON ?= python3
HDAL_INCLUDE_DIR ?=
IWAD ?=

WOOF_DIR := .deps/woof
E1_OPL_DIR := .deps/rp2040-doom/opl
E1_PLATFORM_DIR := src/platform/e1
E1_COMMON_DIR := src/common

WOOF_EXCLUDED := \
	$(WOOF_DIR)/src/i_3dsound.c \
	$(WOOF_DIR)/src/i_endoom.c \
	$(WOOF_DIR)/src/i_flickstick.c \
	$(WOOF_DIR)/src/i_flmusic.c \
	$(WOOF_DIR)/src/i_gamepad.c \
	$(WOOF_DIR)/src/i_gyro.c \
	$(WOOF_DIR)/src/i_input.c \
	$(WOOF_DIR)/src/i_main.c \
	$(WOOF_DIR)/src/i_mbfsound.c \
	$(WOOF_DIR)/src/i_midimusic.c \
	$(WOOF_DIR)/src/i_oalequalizer.c \
	$(WOOF_DIR)/src/i_oalmusic.c \
	$(WOOF_DIR)/src/i_oalsound.c \
	$(WOOF_DIR)/src/i_pcsound.c \
	$(WOOF_DIR)/src/i_rumble.c \
	$(WOOF_DIR)/src/i_sndfile.c \
	$(WOOF_DIR)/src/i_sound.c \
	$(WOOF_DIR)/src/i_system.c \
	$(WOOF_DIR)/src/i_timer.c \
	$(WOOF_DIR)/src/i_video.c \
	$(WOOF_DIR)/src/i_xmp.c \
	$(WOOF_DIR)/src/icon.c \
	$(WOOF_DIR)/src/midifallback.c \
	$(WOOF_DIR)/src/midiout.c \
	$(WOOF_DIR)/src/net_gui.c \
	$(WOOF_DIR)/src/net_sdl.c
WOOF_CORE_SOURCES := $(filter-out $(WOOF_EXCLUDED),$(wildcard $(WOOF_DIR)/src/*.c))
WOOF_PLATFORM_SOURCES := $(wildcard $(E1_PLATFORM_DIR)/*.c)
WOOF_COMMON_SOURCES := $(wildcard $(E1_COMMON_DIR)/*.c)
WOOF_THIRD_PARTY_SOURCES := \
	$(WOOF_DIR)/third-party/md5/md5.c \
	$(WOOF_DIR)/third-party/miniz/miniz.c \
	$(WOOF_DIR)/third-party/sha1/sha1.c \
	$(WOOF_DIR)/third-party/spng/spng.c \
	$(WOOF_DIR)/third-party/yyjson/yyjson.c
WOOF_OPL_SOURCES := \
	$(WOOF_DIR)/opl/opl.c \
	$(WOOF_DIR)/opl/opl_queue.c \
	$(WOOF_DIR)/opl/opl_sdl.c \
	$(E1_OPL_DIR)/emu8950.c
WOOF_SOURCES := $(WOOF_CORE_SOURCES) $(WOOF_PLATFORM_SOURCES) \
	$(WOOF_COMMON_SOURCES) $(WOOF_THIRD_PARTY_SOURCES) $(WOOF_OPL_SOURCES)
WOOF_HEADERS := $(wildcard $(WOOF_DIR)/src/*.h) \
	$(wildcard $(WOOF_DIR)/opl/*.h) \
	$(wildcard $(E1_OPL_DIR)/*.h) \
	$(wildcard $(E1_PLATFORM_DIR)/*.h) $(wildcard $(E1_COMMON_DIR)/*.h)
WOOF_INCLUDES := \
	-I$(E1_PLATFORM_DIR) \
	-I$(E1_COMMON_DIR) \
	-I$(WOOF_DIR)/src \
	-I$(WOOF_DIR)/opl \
	-I$(E1_OPL_DIR) \
	-I$(WOOF_DIR)/third-party/md5 \
	-I$(WOOF_DIR)/third-party/miniz \
	-I$(WOOF_DIR)/third-party/sha1 \
	-I$(WOOF_DIR)/third-party/spng \
	-I$(WOOF_DIR)/third-party/yyjson
WOOF_DEFINES := -DE1_CAMERA -DOPL_SAMPLE_RATE=16000 \
	-DE1_OPL2_ONLY=1 -DUSE_EMU8950_OPL=1 \
	-DEMU8950_NO_RATECONV=1 -DEMU8950_NO_WAVE_TABLE_MAP=1 \
	-DEMU8950_NO_TLL=1 -DEMU8950_NO_FLOAT=1 -DEMU8950_NO_TIMER=1 \
	-DEMU8950_NO_TEST_FLAG=1 -DEMU8950_SIMPLER_NOISE=1 \
	-DEMU8950_SHORT_NOISE_UPDATE_CHECK=1 -DEMU8950_LINEAR_SKIP=1 \
	-DEMU8950_LINEAR_END_OF_NOTE_OPTIMIZATION=1 \
	-DEMU8950_NO_PERCUSSION_MODE=1 -DEMU8950_LINEAR=1 \
	-DEMU8950_SAMPLE_STRIDE=4 \
	-DEMU8950_ASM=0 -DEMU8950_SLOT_RENDER=1 \
	-DPICO_ON_DEVICE=0 -DLIB_PICO_PLATFORM=0 \
	-DSPNG_STATIC -DSPNG_USE_MINIZ -DMINIZ_NO_TIME \
	-DYYJSON_DISABLE_WRITER=1 -DYYJSON_DISABLE_UTILS=1 \
	-DBUILD_DATE=\"2026-08-31\"
WOOF_CFLAGS := -std=gnu11 -O3 -g0 -w -pthread -fms-extensions \
	-fomit-frame-pointer -fno-ident $(WOOF_INCLUDES) $(WOOF_DEFINES)
WOOF_CXXFLAGS := -std=gnu++17 -O3 -g0 -w -pthread -fms-extensions \
	-fomit-frame-pointer -fno-ident $(WOOF_INCLUDES) $(WOOF_DEFINES)
WOOF_PLATFORM_CHECK_CFLAGS := -std=gnu11 -Wall -Wextra -Werror \
	-Wno-unused-parameter $(WOOF_INCLUDES) $(WOOF_DEFINES)
COMMON_CFLAGS := -std=c11 -O3 -g0 -Wall -Wextra -Werror \
	-mfpu=neon-vfpv4 -mfloat-abi=hard -fomit-frame-pointer -fno-ident

.PHONY: all deps camera clean host-check integration-check check _host-check \
	_check package corresponding-source check-hdal frame_convert_test \
	ptz_input_test runtime_control_test melt_test audio_contract_test \
	audio_ring_test sfx_mixer_test opl_music_host_test \
	opl2_equivalence_test http_controller_host_test runtime_fps_parser_test \
	device_signature_test boot_service_test

all: deps
	+$(MAKE) camera

deps:
	tools/fetch-sources

camera: check-hdal $(BUILD_DIR)/e1-doom $(BUILD_DIR)/e1-doom-controller \
	$(BUILD_DIR)/e1-doom-probe $(BUILD_DIR)/e1-doom-injector \
	$(BUILD_DIR)/e1-doom-injected.so $(BUILD_DIR)/e1-ptz-patch-supervisor \
	$(BUILD_DIR)/e1-ptz-hook-owner-dummy $(BUILD_DIR)/woof.pk3

check-hdal:
	@test -n "$(HDAL_INCLUDE_DIR)" || { \
		echo 'Set HDAL_INCLUDE_DIR to the vendor HDAL headers.' >&2; exit 2; \
	}
	@test -s "$(HDAL_INCLUDE_DIR)/hdal.h" || { \
		echo 'HDAL_INCLUDE_DIR does not contain hdal.h.' >&2; exit 2; \
	}

host-check: deps
	+$(MAKE) _host-check

_host-check: frame_convert_test ptz_input_test runtime_control_test melt_test \
	audio_contract_test audio_ring_test sfx_mixer_test \
	opl2_equivalence_test runtime_fps_parser_test device_signature_test \
	boot_service_test

integration-check: deps
	@test -s "$(IWAD)" || { echo 'Set IWAD to a Doom IWAD.' >&2; exit 2; }
	+$(MAKE) opl_music_host_test http_controller_host_test IWAD="$(IWAD)"

check: deps
	+$(MAKE) _check

_check: camera _host-check $(BUILD_DIR)/e1-frame-expand-arm-test
	$(HOST_CC) $(WOOF_PLATFORM_CHECK_CFLAGS) -fsyntax-only $(WOOF_PLATFORM_SOURCES)
	@for binary in e1-doom e1-doom-controller e1-doom-probe e1-doom-injector \
		e1-doom-injected.so e1-ptz-patch-supervisor e1-ptz-hook-owner-dummy; do \
		file "$(BUILD_DIR)/$$binary" | grep -q 'ARM.*EABI5' || { \
			echo "$$binary is not ARM EABI5" >&2; exit 1; }; \
		$(READELF) -h "$(BUILD_DIR)/$$binary" | \
			grep -q 'Flags:.*0x5000400' || { \
			echo "$$binary is not ARM hard-float" >&2; exit 1; }; \
	done
	@if $(READELF) -d $(BUILD_DIR)/e1-doom | \
		grep -Eqi 'SDL|openal|asound|fluidsynth'; then \
		echo 'e1-doom retains a desktop dependency' >&2; exit 1; \
	fi

package: deps
	@test -s "$(IWAD)" || { echo 'Set IWAD to a Doom IWAD.' >&2; exit 2; }
	+$(MAKE) camera HDAL_INCLUDE_DIR="$(HDAL_INCLUDE_DIR)"
	BUILD_DIR="$(abspath $(BUILD_DIR))" IWAD="$(IWAD)" tools/package-flat

corresponding-source: deps
	tools/corresponding-source

frame_convert_test: $(BUILD_DIR)/e1-frame-convert-test
	$(BUILD_DIR)/e1-frame-convert-test
ptz_input_test: $(BUILD_DIR)/e1-ptz-input-test
	$(BUILD_DIR)/e1-ptz-input-test
runtime_control_test: $(BUILD_DIR)/e1-runtime-control-test
	$(BUILD_DIR)/e1-runtime-control-test
melt_test: $(BUILD_DIR)/e1-melt-test
	$(BUILD_DIR)/e1-melt-test
audio_contract_test: $(BUILD_DIR)/e1-audio-contract-test
	$(BUILD_DIR)/e1-audio-contract-test
audio_ring_test: $(BUILD_DIR)/e1-audio-ring-test
	$(BUILD_DIR)/e1-audio-ring-test
sfx_mixer_test: $(BUILD_DIR)/e1-sfx-mixer-test
	$(BUILD_DIR)/e1-sfx-mixer-test
opl2_equivalence_test: $(BUILD_DIR)/e1-opl2-equivalence-test
	$(BUILD_DIR)/e1-opl2-equivalence-test
runtime_fps_parser_test:
	src/tests/e1-runtime-fps-parser-host-test.sh target/e1-doom-runtime
device_signature_test: $(BUILD_DIR)/e1-device-signature-test
	$(BUILD_DIR)/e1-device-signature-test
boot_service_test:
	/bin/sh -n target/e1-doom-boot
	/bin/sh src/tests/e1-doom-boot-host-test.sh target/e1-doom-boot

opl_music_host_test: $(BUILD_DIR)/e1-doom-host $(BUILD_DIR)/woof.pk3
	src/tests/e1-opl-music-host-test.sh $(BUILD_DIR)/e1-doom-host "$(IWAD)"
http_controller_host_test: $(BUILD_DIR)/e1-doom-host \
	$(BUILD_DIR)/e1-doom-controller-host $(BUILD_DIR)/woof.pk3
	src/tests/e1-http-controller-host-test.sh $(BUILD_DIR)/e1-doom-host \
		"$(IWAD)" $(BUILD_DIR)/e1-doom-controller-host

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/e1-frame-convert-test: src/tests/e1-frame-convert-test.c \
	$(wildcard $(E1_COMMON_DIR)/*.h) | $(BUILD_DIR)
	$(HOST_CC) -std=c11 -O2 -Wall -Wextra -Werror -I$(E1_COMMON_DIR) -o $@ $<
$(BUILD_DIR)/e1-ptz-input-test: src/tests/e1-ptz-input-test.c \
	$(E1_COMMON_DIR)/e1-ptz-input.h | $(BUILD_DIR)
	$(HOST_CC) -std=c11 -O2 -Wall -Wextra -Werror -I$(E1_COMMON_DIR) -o $@ $<
$(BUILD_DIR)/e1-runtime-control-test: src/tests/e1-runtime-control-test.c \
	$(E1_COMMON_DIR)/e1-runtime-control.h | $(BUILD_DIR)
	$(HOST_CC) -std=c11 -O2 -Wall -Wextra -Werror -I$(E1_COMMON_DIR) -o $@ $<
$(BUILD_DIR)/e1-melt-test: src/tests/e1-melt-test.c $(E1_COMMON_DIR)/e1-melt.h | $(BUILD_DIR)
	$(HOST_CC) -std=c11 -O2 -Wall -Wextra -Werror -I$(E1_COMMON_DIR) -o $@ $<
$(BUILD_DIR)/e1-bogomips: src/tests/e1-bogomips.c | $(BUILD_DIR)
	$(HOST_CC) -std=c11 -O2 -Wall -Wextra -Werror -pthread -o $@ $<
$(BUILD_DIR)/e1-audio-contract-test: src/tests/e1-audio-contract-test.c \
	$(E1_COMMON_DIR)/e1-audio-contract.h | $(BUILD_DIR)
	$(HOST_CC) -std=c11 -O2 -Wall -Wextra -Werror -I$(E1_COMMON_DIR) -o $@ $<
$(BUILD_DIR)/e1-audio-ring-test: src/tests/e1-audio-ring-test.c \
	$(E1_COMMON_DIR)/e1-audio-ring.c $(E1_COMMON_DIR)/e1-audio-ring.h \
	$(E1_COMMON_DIR)/e1-audio-contract.h | $(BUILD_DIR)
	$(HOST_CC) -std=c11 -O2 -Wall -Wextra -Werror -I$(E1_COMMON_DIR) \
		-o $@ src/tests/e1-audio-ring-test.c $(E1_COMMON_DIR)/e1-audio-ring.c
$(BUILD_DIR)/e1-sfx-mixer-test: src/tests/e1-sfx-mixer-test.c \
	$(E1_COMMON_DIR)/e1-sfx-mixer.c $(E1_COMMON_DIR)/e1-sfx-mixer.h \
	$(E1_COMMON_DIR)/e1-audio-contract.h | $(BUILD_DIR)
	$(HOST_CC) -std=c11 -O2 -Wall -Wextra -Werror -I$(E1_COMMON_DIR) \
		-o $@ src/tests/e1-sfx-mixer-test.c $(E1_COMMON_DIR)/e1-sfx-mixer.c
$(BUILD_DIR)/e1-device-signature-test: src/tests/e1-device-signature-test.c \
	$(E1_COMMON_DIR)/e1-device-functions.h $(E1_COMMON_DIR)/e1-device-signatures.h | $(BUILD_DIR)
	$(HOST_CC) -std=c11 -O2 -Wall -Wextra -Werror -I$(E1_COMMON_DIR) -o $@ $<

OPL_REFERENCE_RENAMES := \
	-DOPL3_Generate=OPL3_ReferenceGenerate \
	-DOPL3_GenerateResampled=OPL3_ReferenceGenerateResampled \
	-DOPL3_Reset=OPL3_ReferenceReset \
	-DOPL3_WriteReg=OPL3_ReferenceWriteReg \
	-DOPL3_WriteRegBuffered=OPL3_ReferenceWriteRegBuffered \
	-DOPL3_GenerateStream=OPL3_ReferenceGenerateStream
$(BUILD_DIR)/e1-opl3-fast.o: $(WOOF_DIR)/opl/opl3.c $(WOOF_DIR)/opl/opl3.h \
	$(E1_COMMON_DIR)/e1-opl2-noise.h | $(BUILD_DIR)
	$(HOST_CC) -std=c11 -O3 -Wall -Wextra -Werror -I$(E1_COMMON_DIR) \
		-I$(WOOF_DIR)/opl -DE1_OPL2_FAST_PATH=1 -DE1_OPL2_ONLY=1 -c -o $@ $<
$(BUILD_DIR)/e1-opl3-reference.o: $(WOOF_DIR)/opl/opl3.c $(WOOF_DIR)/opl/opl3.h \
	$(E1_COMMON_DIR)/e1-opl2-noise.h | $(BUILD_DIR)
	$(HOST_CC) -std=c11 -O3 -Wall -Wextra -Werror -I$(E1_COMMON_DIR) \
		-I$(WOOF_DIR)/opl -DE1_OPL2_FAST_PATH=0 $(OPL_REFERENCE_RENAMES) -c -o $@ $<
$(BUILD_DIR)/e1-opl2-equivalence-test: src/tests/e1-opl2-equivalence-test.c \
	$(BUILD_DIR)/e1-opl3-fast.o $(BUILD_DIR)/e1-opl3-reference.o | $(BUILD_DIR)
	$(HOST_CC) -std=c11 -O3 -Wall -Wextra -Werror -I$(E1_COMMON_DIR) \
		-I$(WOOF_DIR)/opl -o $@ $^

$(BUILD_DIR)/woof.pk3: $(shell find $(WOOF_DIR)/base -type f 2>/dev/null) \
	tools/build-woof-pk3 | $(BUILD_DIR)
	$(PYTHON) tools/build-woof-pk3 $(WOOF_DIR)/base $@
$(BUILD_DIR)/e1-opl-slot-render-host.o: $(E1_OPL_DIR)/slot_render.cpp \
	$(E1_OPL_DIR)/slot_render.h | $(BUILD_DIR)
	$(HOST_CXX) $(WOOF_CXXFLAGS) -c -o $@ $<
$(BUILD_DIR)/e1-opl-slot-render-arm.o: $(E1_OPL_DIR)/slot_render.cpp \
	$(E1_OPL_DIR)/slot_render.h | $(BUILD_DIR)
	$(ZIG) c++ -target $(TARGET) -mcpu=$(CPU) $(WOOF_CXXFLAGS) \
		-mfpu=neon-vfpv4 -mfloat-abi=hard -c -o $@ $<
$(BUILD_DIR)/e1-doom-host: $(WOOF_SOURCES) $(WOOF_HEADERS) \
	$(BUILD_DIR)/e1-opl-slot-render-host.o | $(BUILD_DIR)
	$(HOST_CC) $(WOOF_CFLAGS) -o $@ $(WOOF_SOURCES) \
		$(BUILD_DIR)/e1-opl-slot-render-host.o -lm
$(BUILD_DIR)/e1-doom: $(WOOF_SOURCES) $(WOOF_HEADERS) \
	$(BUILD_DIR)/e1-opl-slot-render-arm.o | $(BUILD_DIR)
	$(ZIG) cc -target $(TARGET) -mcpu=$(CPU) $(WOOF_CFLAGS) \
		-mfpu=neon-vfpv4 -mfloat-abi=hard -Wl,--build-id=sha1 -s \
		-o $@ $(WOOF_SOURCES) $(BUILD_DIR)/e1-opl-slot-render-arm.o -lm
$(BUILD_DIR)/e1-doom-controller: $(E1_PLATFORM_DIR)/i_input_e1.c \
	$(E1_COMMON_DIR)/e1-key-event.h | $(BUILD_DIR)
	$(ZIG) cc -target $(TARGET) -mcpu=$(CPU) $(COMMON_CFLAGS) \
		-I$(E1_PLATFORM_DIR) -I$(E1_COMMON_DIR) -I$(WOOF_DIR)/src \
		-DE1_STANDALONE_CONTROLLER=1 -Wl,--build-id=sha1 -s -o $@ $<
$(BUILD_DIR)/e1-doom-controller-host: $(E1_PLATFORM_DIR)/i_input_e1.c \
	$(E1_COMMON_DIR)/e1-key-event.h | $(BUILD_DIR)
	$(HOST_CC) -std=c11 -O2 -Wall -Wextra -Werror -I$(E1_PLATFORM_DIR) \
		-I$(E1_COMMON_DIR) -I$(WOOF_DIR)/src -DE1_STANDALONE_CONTROLLER=1 -o $@ $<
$(BUILD_DIR)/e1-doom-probe: src/camera/e1-doom-probe.c | $(BUILD_DIR)
	$(ZIG) cc -target $(TARGET) -mcpu=$(CPU) $(COMMON_CFLAGS) \
		-Wl,--build-id=sha1 -s -o $@ $< -ldl
$(BUILD_DIR)/e1-doom-injector: src/camera/e1-doom-injector.c | $(BUILD_DIR)
	$(ZIG) cc -target $(TARGET) -mcpu=$(CPU) $(COMMON_CFLAGS) \
		-Wl,--build-id=sha1 -s -o $@ $<
$(BUILD_DIR)/e1-doom-injected.so: src/camera/e1-doom-injected.c \
	src/camera/e1-ptz-hook.S src/camera/e1-audio-hook.S \
	$(E1_COMMON_DIR)/e1-device-signatures.h $(E1_COMMON_DIR)/e1-frame-expand-arm.S \
	$(E1_COMMON_DIR)/e1-audio-ring.c $(wildcard $(E1_COMMON_DIR)/*.h) | $(BUILD_DIR)
	$(ZIG) cc -target $(TARGET) -mcpu=$(CPU) $(COMMON_CFLAGS) -fPIC -shared \
		-I$(HDAL_INCLUDE_DIR) -I$(E1_COMMON_DIR) -Wl,--build-id=sha1 \
		-Wl,-soname,e1-doom-injected.so -s -o $@ \
		src/camera/e1-doom-injected.c src/camera/e1-ptz-hook.S \
		src/camera/e1-audio-hook.S $(E1_COMMON_DIR)/e1-frame-expand-arm.S \
		$(E1_COMMON_DIR)/e1-audio-ring.c
$(BUILD_DIR)/e1-frame-expand-arm-test: src/tests/e1-frame-expand-arm-test.c \
	$(E1_COMMON_DIR)/e1-frame-expand-arm.S $(E1_COMMON_DIR)/e1-frame-expand.h | $(BUILD_DIR)
	$(ZIG) cc -target $(TARGET) -mcpu=$(CPU) $(COMMON_CFLAGS) -I$(E1_COMMON_DIR) \
		-Wl,--build-id=sha1 -s -o $@ src/tests/e1-frame-expand-arm-test.c \
		$(E1_COMMON_DIR)/e1-frame-expand-arm.S
$(BUILD_DIR)/e1-ptz-patch-supervisor: src/camera/e1-ptz-patch-supervisor.c \
	$(E1_COMMON_DIR)/e1-device-signatures.h | $(BUILD_DIR)
	$(ZIG) cc -target $(TARGET) -mcpu=$(CPU) $(COMMON_CFLAGS) \
		-I$(E1_COMMON_DIR) -Wl,--build-id=sha1 -s -o $@ $<
$(BUILD_DIR)/e1-ptz-hook-owner-dummy: src/camera/e1-ptz-hook-owner-dummy.c | $(BUILD_DIR)
	$(ZIG) cc -target $(TARGET) -mcpu=$(CPU) $(COMMON_CFLAGS) \
		-Wl,--build-id=sha1 -s -o $@ $<

clean:
	rm -rf "$(BUILD_DIR)" dist
