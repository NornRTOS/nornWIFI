# nw02: join the access point named in nw_secrets.h (WPA2 personal or open), get an
# address by DHCP through nornNET, ping the gateway and fetch a page from the host.
# Board only. Copy nw_secrets.example.h to nw_secrets.h (ignored by git) first.
BOARD        = esp32
ONLY_BOARDS  = esp32
NW_BOARD_ONLY = 1
NW_MON_SECS  = 40
NORNNET_DIR  = $(abspath $(dir $(lastword $(MAKEFILE_LIST)))../../../nornNET)
MODULES     += $(NORNNET_DIR)/nornnet.mk
NORNWIF_DIR  = $(abspath $(dir $(lastword $(MAKEFILE_LIST)))../..)
MODULES     += $(NORNWIF_DIR)/nornwif.mk
# Nothing of nornNET, libgcc or libc runs in an interrupt, so all of it runs from flash
# (the cache is never disabled); only the kernel and the adapter stay in IRAM.
FLASH_OBJS  += *nn_*.o *libgcc.a *libc.a
