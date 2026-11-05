# nw05: the soak for the stability gate over the radio: joins the network of
# apps/nw02/nw_secrets.h and runs transfers through tools/tcp_sink.py on the host plus
# pings forever, one line of counters per round. Driven by tools/soak_wifi.sh; not part
# of test-all (NW_SKIP_TEST). Board only.
BOARD        = esp32
ONLY_BOARDS  = esp32
NW_BOARD_ONLY = 1
NW_SKIP_TEST = 1
NORNNET_DIR  = $(abspath $(dir $(lastword $(MAKEFILE_LIST)))../../../nornNET)
MODULES     += $(NORNNET_DIR)/nornnet.mk
NORNWIF_DIR  = $(abspath $(dir $(lastword $(MAKEFILE_LIST)))../..)
MODULES     += $(NORNWIF_DIR)/nornwif.mk
FLASH_OBJS  += *nn_*.o *libgcc.a *libc.a
