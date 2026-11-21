# Standalone PS5 guest-window probe; does not enable the ReLink PS5 target.
# Usage: PS5_PAYLOAD_SDK=/path/to/pinned/sdk make -C tools/ps5

EXPECTED_SDK_REVISION := b83202be73e930050e00de0f1b4d9ec46c0391bf
SDK_REVISION_FILE := ../../deps/ps5-payload-sdk.commit

ifndef PS5_PAYLOAD_SDK
$(error PS5_PAYLOAD_SDK must point to the installed pinned PS5 Payload SDK)
endif

ifeq ($(wildcard $(PS5_PAYLOAD_SDK)/bin/prospero-clang),)
$(error PS5_PAYLOAD_SDK is missing bin/prospero-clang)
endif

ACTUAL_SDK_REVISION := $(shell test -f "$(PS5_PAYLOAD_SDK)/.ps5-sdk-revision" && tr -d '\r\n' < "$(PS5_PAYLOAD_SDK)/.ps5-sdk-revision")
ifneq ($(ACTUAL_SDK_REVISION),$(EXPECTED_SDK_REVISION))
$(error SDK revision mismatch: expected $(EXPECTED_SDK_REVISION), found $(ACTUAL_SDK_REVISION))
endif

CC := $(PS5_PAYLOAD_SDK)/bin/prospero-clang
CPPFLAGS := -I$(PS5_PAYLOAD_SDK)/target/include
CFLAGS ?= -O2 -Wall -Wextra -Werror
TARGET := relink-guest-window-probe.elf
SOURCES := guest_window_probe_main.c guest_window_probe.c

.PHONY: all clean
all: $(TARGET)

$(TARGET): $(SOURCES)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -o $@ $(SOURCES)

clean:
	rm -f $(TARGET)
