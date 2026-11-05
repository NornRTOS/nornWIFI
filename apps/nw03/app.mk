# nw03: receiver check. Listens in promiscuous mode on NW03_CHANNEL (default 10) for
# NW03_SECONDS and prints how many frames arrived per second and per rate class (11b,
# 11g, 11n), with beacons counted separately: a healthy receiver sees every beacon of
# every access point on the channel, ten per second each, at 1 Mbit/s 11b. Board only.
BOARD        = esp32
ONLY_BOARDS  = esp32
NW_BOARD_ONLY = 1
NW_MON_SECS  = 20
NORNWIF_DIR  = $(abspath $(dir $(lastword $(MAKEFILE_LIST)))../..)
MODULES     += $(NORNWIF_DIR)/nornwif.mk
