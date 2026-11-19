# nornWIF: everything is built by the nornOS Makefile with APPDIR pointing at an
# application here (apps/nwNN); this file only forwards the common targets.
#
#   make nw01            build apps/nw01
#   make nw01-upload     flash it (PORT=/dev/ttyUSB0)
#   make nw01-monitor    watch it
#   make test-all        every application on the board (tools/test_all.sh)
NORNOS_DIR ?= $(abspath ../nornCore)

NW_APPS := $(notdir $(wildcard apps/nw*))
PORT ?= /dev/ttyUSB0

.PHONY: all test-all clean $(NW_APPS) $(NW_APPS:%=%-upload) $(NW_APPS:%=%-monitor)

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
