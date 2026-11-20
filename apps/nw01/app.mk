# nw01: the radio under nornOS on the real ESP32: initialise, start the station, scan
# for access points and print them. Board only; QEMU has no radio.
BOARD        = esp32
ONLY_BOARDS  = esp32
NW_BOARD_ONLY = 1
NW_MON_SECS  = 25
NORNWIF_DIR  = $(abspath $(dir $(lastword $(MAKEFILE_LIST)))../..)
MODULES     += $(NORNWIF_DIR)/nornwif.mk
