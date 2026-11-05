# nornWIF: everything is built by the nornOS Makefile with APPDIR pointing at an
# application here (apps/nwNN); this file only forwards the common targets.
#
#   make nw01            build apps/nw01
#   make nw01-upload     flash it (PORT=/dev/ttyUSB0)
#   make nw01-monitor    watch it
#   make test-all        every application on the board (tools/test_all.sh)
#   make lib PROFILE=default
#                        the module library of the binary SDK (and libnwsupp.a, the
#                        supplicant), built against the nornRTOS profile (PROFILE_DIR)
NORNOS_DIR ?= $(abspath ../nornCore)
NORNNET_DIR ?= $(abspath ../nornNET)
NORN_RTOS ?= $(abspath ../..)
PROFILE ?= default
PROFILE_DIR ?= $(NORN_RTOS)/profiles/$(PROFILE)
lib:
	@test -f $(PROFILE_DIR)/nw_config.h || { echo "no nw_config.h in $(PROFILE_DIR)"; exit 1; }
	$(MAKE) --no-print-directory -C $(NORNOS_DIR) modlib \
	        MODULES="$(abspath $(NORNNET_DIR)/nornnet.mk) $(abspath nornwif.mk)" NORNNET_DIR=$(abspath $(NORNNET_DIR)) \
	        MODNAME=nornWIFI MODDIR=$(abspath .) MOD_PROFILE_DIR=$(abspath $(PROFILE_DIR)) PROFILE_DIR=$(abspath $(PROFILE_DIR)) \
	        BOARD=esp32 PROFILE=$(PROFILE)

NW_APPS := $(notdir $(wildcard apps/nw*))
PORT ?= /dev/ttyUSB0

.PHONY: all test-all clean lib $(NW_APPS) $(NW_APPS:%=%-upload) $(NW_APPS:%=%-monitor)

all: $(NW_APPS)

$(NW_APPS): %:
	$(MAKE) -s -C $(NORNOS_DIR) APP=$@ APPDIR=$(abspath apps/$@) BOARD=esp32 image

$(NW_APPS:%=%-upload): %-upload:
	$(MAKE) -s -C $(NORNOS_DIR) APP=$* APPDIR=$(abspath apps/$*) BOARD=esp32 PORT=$(PORT) upload

$(NW_APPS:%=%-monitor): %-monitor:
	$(MAKE) -s -C $(NORNOS_DIR) APP=$* APPDIR=$(abspath apps/$*) BOARD=esp32 PORT=$(PORT) monitor

test-all:
	tools/test_all.sh

clean:
	rm -rf build $(NORNOS_DIR)/build/nw*-esp32
