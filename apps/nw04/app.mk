# nw04: automatic reconnection. Joins the network of apps/nw02/nw_secrets.h, drops the
# association three times behind the policy's back and checks that the station comes
# back by itself with DHCP confirmed, pings answered and a TCP connection intact.
# NW04_MANUAL=1 (EXTRA_CFLAGS) adds a two minute window to switch the access point off
# and on. Board only, needs nw02's nw_secrets.h.
BOARD        = esp32
ONLY_BOARDS  = esp32
NW_BOARD_ONLY = 1
NW_MON_SECS  = 60
NORNNET_DIR  = $(abspath $(dir $(lastword $(MAKEFILE_LIST)))../../../nornNET)
MODULES     += $(NORNNET_DIR)/nornnet.mk
NORNWIF_DIR  = $(abspath $(dir $(lastword $(MAKEFILE_LIST)))../..)
MODULES     += $(NORNWIF_DIR)/nornwif.mk
FLASH_OBJS  += *nn_*.o *libgcc.a *libc.a
