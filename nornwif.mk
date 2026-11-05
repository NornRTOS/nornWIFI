# nornWIF as a module of a nornOS application (ESP32 only). The application's app.mk says
#
#   NORNWIF_DIR = /path/to/nornWIF
#   MODULES    += $(NORNWIF_DIR)/nornwif.mk
#
# after nornNET's fragment if the stack is used too (nw_netdev.c is then compiled). The
# radio libraries and the glue that is not interrupt or timing critical run from flash
# (nornOS FLASH_OBJS); the parts Espressif marks for IRAM stay in IRAM. An nw_config.h
# next to the application overrides the NW_CFG_* defaults.
NW_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))

ifneq ($(BOARD),esp32)
  $(error nornWIF only runs on BOARD=esp32)
endif

MOD_SRC    += $(wildcard $(NW_DIR)/nw/*.c)
MOD_INC    += -I$(NW_DIR)/nw -isystem $(NW_DIR)/include -isystem $(NW_DIR)/include/esp_private -isystem $(NW_DIR)/include/stubs
MOD_CFLAGS += -DNW_NORNOS
ifneq ($(wildcard $(APPDIR)/nw_config.h),)
  MOD_CFLAGS += -DNW_CONFIG_H
else ifneq ($(wildcard $(PROFILE_DIR)/nw_config.h),)
  MOD_CFLAGS += -DNW_CONFIG_H          # the nornRTOS profile's nw_config.h, on the include path
endif
ifdef NORNNET_DIR
  MOD_CFLAGS += -DNW_CFG_NORNNET=1
endif

# The ROM functions Espressif's binary libraries (lib/esp32/VERSION says which) use; the
# libraries themselves are linked below together with the supplicant.
MOD_LD     += $(NW_DIR)/port/esp32_rom_wifi.ld

# Placement (see nornOS esp32.ld): everything of the libraries in flash except the
# sections Espressif keeps in IRAM by default (ESP_WIFI_IRAM_OPT, RX_IRAM_OPT); the sleep
# related IRAM sections go to flash as with the default configuration. Log strings are
# constants in flash. Of our own code only the adapter stays in IRAM.
#
# Memory knobs, on the make command line or in app.mk (doc/plan.md has the measurements):
#   NW_IRAM_OPT=0  the library's hot paths (.wifi0iram, .wifirxiram, .wifiextrairam,
#                  about 25 KB) run from flash too, as with ESP_WIFI_IRAM_OPT=n in ESP-IDF;
#                  the cache is never disabled under nornOS so this is safe, only slower.
#   NW_SMALL=1     the small buffer profile of nw_config.h (fewer RX/TX buffers, no AMPDU).
NW_IRAM_OPT ?= 1
NW_SMALL    ?= 0
MOD_FLASH_OBJS += *libnet80211.a *libpp.a *libcore.a *libphy.a *librtc.a \
                  *nw_wifi.o *nw_phy.o *nw_printf.o *nw_regulatory.o *nw_phy_init_data.o
NW_HOT_SECTIONS := .wifi0iram .wifi0iram.* .wifirxiram .wifirxiram.* .wifiextrairam .wifiextrairam.*
ifeq ($(NW_IRAM_OPT),0)
  MOD_FLASH_SECTIONS += $(NW_HOT_SECTIONS)
  MOD_CFLAGS += -DNW_CFG_IRAM_OPT=0
else
  MOD_IRAM_SECTIONS += $(NW_HOT_SECTIONS)
endif
ifeq ($(NW_SMALL),1)
  MOD_CFLAGS += -DNW_CFG_BUFFERS_SMALL=1
endif
MOD_FLASH_SECTIONS += .wifislpiram .wifislpiram.* .wifiorslpiram .wifiorslpiram.* \
                      .wifislprxiram .wifislprxiram.* .phyiram .phyiram.* .coexiram .coexiram.* \
                      .coexsleepiram .coexsleepiram.*
MOD_FLASH_RODATA_SECTIONS += .rodata_wlog_error .rodata_wlog_error.* .rodata_wlog_warning .rodata_wlog_warning.* \
                             .rodata_wlog_info .rodata_wlog_info.* .rodata_wlog_debug .rodata_wlog_debug.* \
                             .rodata_wlog_verbose .rodata_wlog_verbose.*

# The WPA supplicant for the station, ESP-IDF's port of wpa_supplicant (supplicant/,
# BSD licence), compiled here into its own library with the defines ESP-IDF uses, less
# WPS, WPA3, enterprise EAP and the soft AP: WPA and WPA2 personal with PMF. The sources
# are compiled as they are, warnings off, so they keep their own include layout.
SUPP_DIR  := $(NW_DIR)/supplicant
SUPP_SRC  := esp_supplicant/src/esp_wpa_main.c esp_supplicant/src/esp_wpas_glue.c \
             esp_supplicant/src/esp_common.c \
             port/os_xtensa.c port/eloop.c \
             src/rsn_supp/wpa.c src/rsn_supp/wpa_ie.c src/rsn_supp/pmksa_cache.c \
             src/common/wpa_common.c src/common/ieee802_11_common.c \
             src/utils/common.c src/utils/wpabuf.c src/utils/wpa_debug.c \
             src/crypto/crypto_ops.c src/crypto/crypto_internal.c \
             src/crypto/sha1.c src/crypto/sha1-internal.c src/crypto/sha1-pbkdf2.c src/crypto/sha1-prf.c \
             src/crypto/sha256.c src/crypto/sha256-internal.c src/crypto/sha256-prf.c src/crypto/sha256-kdf.c \
             src/crypto/aes-internal.c src/crypto/aes-internal-enc.c src/crypto/aes-internal-dec.c \
             src/crypto/aes-unwrap.c src/crypto/aes-wrap.c src/crypto/aes-omac1.c src/crypto/aes-ccm.c \
             src/crypto/md5.c src/crypto/md5-internal.c src/crypto/rc4.c src/crypto/aes-cbc.c src/crypto/ccmp.c
SUPP_OBJ  := $(patsubst %.c,$(BUILD)/mod/nwsupp/%.o,$(SUPP_SRC))
SUPP_LIB  := $(BUILD)/mod/libnwsupp.a
SUPP_DEFS := -DESP_PLATFORM=1 -D__ets__ -DESP_SUPPLICANT -DIEEE8021X_EAPOL -DESPRESSIF_USE -DCONFIG_IEEE80211W \
             -DCONFIG_SHA256 -DCONFIG_NO_RADIUS -DCONFIG_CRYPTO_INTERNAL
SUPP_INC  := -isystem $(NW_DIR)/include/stubs -I$(SUPP_DIR)/port/include -I$(SUPP_DIR)/include \
             -I$(SUPP_DIR)/esp_supplicant/include -I$(SUPP_DIR)/esp_supplicant/src \
             -I$(SUPP_DIR)/src -I$(SUPP_DIR)/src/utils -I$(SUPP_DIR)/src/crypto
SUPP_CFLAGS = -std=gnu99 -Os -g -mlongcalls -ffunction-sections -fdata-sections -fno-common -w \
              -DF_CPU=$(F_CPU) -DHAL_APB_HZ=$(APB_HZ) $(SUPP_DEFS) $(SUPP_INC) $(INC) -MMD -MP

$(BUILD)/mod/nwsupp/%.o: $(SUPP_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(SUPP_CFLAGS) -c -o $@ $<

$(SUPP_LIB): $(SUPP_OBJ)
	rm -f $@
	$(AR) rcs $@ $^

-include $(SUPP_OBJ:.o=.d)

MOD_LIBS   += $(SUPP_LIB)
# NW_LIBS: directory of Espressif's binary libraries (default lib/esp32; another version
# can be tried from the command line).
NW_LIBS ?= $(NW_DIR)/lib/esp32
MOD_LDLIBS += -Wl,--start-group $(SUPP_LIB) -L$(NW_LIBS) -lnet80211 -lpp -lcore -lphy -lrtc -Wl,--end-group
MOD_FLASH_OBJS += *libnwsupp.a
