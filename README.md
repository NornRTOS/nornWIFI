# nornWIF

The ESP32's WiFi radio under [nornCore](../nornCore), without FreeRTOS or ESP-IDF. Espressif
only ships the radio as binary libraries written against an operating system adapter
table; nornWIF implements that table over the nornOS primitives, adds the chip glue
(power domain, clocks, PHY calibration, interrupts), the WPA supplicant for WPA2
personal, and a network device for [nornNET](../nornNET). The result is a station that
scans, joins a network, gets an address by DHCP and carries TCP, with nornOS as the
only kernel in the image.

Status 2026-11-03: scanning verified on the NodeMCU-32S (apps/nw01); joining a WPA2
network with the 4-way handshake, DHCP, ping, DNS and an HTTP GET over nornNET TCP
verified against the office access point (apps/nw02, HT20); apps/nw03 measures what
the receiver hears on a channel; apps/nw04 checks that a lost link comes back by
itself. TCP through the sink runs at 400 to 650 kB/s each way. Open items and
measurements in doc/plan.md.

## Layout

    nornwif.mk        the module fragment a nornOS application includes (MODULES +=)
    nw/               our code
      nw.h            the API: nw_init, nw_start, nw_scan, nw_connect/nw_connectStart,
                      link state and reconnection, events, stats
      nw_osi.c        the adapter table (wifi_osi_funcs_t): tasks, queues, semaphores,
                      mutexes, event groups, timers, heap, interrupts, events
      nw_phy.c        power domain, clock gates, resets, PHY init and calibration,
                      efuse MAC, random numbers
      nw_wifi.c       the station: init order, events, scan, connect
      nw_netdev.c     the nornNET device (compiled when NORNNET_DIR is set)
      nw_printf.c     formatting and logging for us and for the libraries
      nw_regulatory.c, nw_phy_init_data.c   tables from ESP-IDF, unchanged
    lib/esp32/        Espressif's binary libraries (Apache-2.0), VERSION says which
    include/          the ESP-IDF headers the libraries' API needs, and stubs/ for
                      what those headers include of the rest of ESP-IDF
    supplicant/       ESP-IDF's wpa_supplicant port (BSD), built as a library by
                      nornwif.mk; station WPA/WPA2 personal only
    port/             ROM symbols the libraries use
    apps/             test applications: nw01 scan, nw02 join + DHCP + ping + HTTP +
                      throughput, nw03 receiver check, nw04 reconnection
    tools/            test_all.sh runs the applications on the board
    doc/plan.md       design, what was measured, what is left

## How it fits nornOS

The radio needs about 350 KB of code, far more than the 128 KB of IRAM, so nornOS
gained the ability to run code from flash through the cache (FLASH_OBJS in a module
fragment, `rtos_port_flashInit` maps the pages before main). The libraries and the
supplicant run from flash; the parts Espressif marks for IRAM, the kernel and the
adapter stay in IRAM. Memory on the board with nw02 (nornOS + nornNET + nornWIF):
311 KB of flash code and constants, 71 KB IRAM, 59 KB of static DRAM, and the radio
takes about 29 KB of heap from the second SRAM bank the nornOS image does not use
(NW_HEAP_SPARE_START, 113 KB), leaving 84 KB there.

Two knobs trade speed for memory when an application needs the room (doc/plan.md has
the figures): `make nwNN NW_IRAM_OPT=0` runs the libraries' hot paths from flash too,
which halves the IRAM the radio takes, and `NW_SMALL=1` selects the small buffer
profile of nw_config.h. An application can also put its own code, nornNET, libgcc and
libc in flash with FLASH_OBJS in app.mk, as nw02 does: with everything in flash the
whole stack needs 24 KB of IRAM.

The libraries create one task ("wifi") and want timers and events delivered in task
context, so nornWIF owns two nornOS tasks: the radio task, which must be the highest
priority task of the application, and a service task below it that runs the timer
callbacks and hands events to the application's callback. nornOS tasks are static, so
both exist from `nw_init` on and the library's task starts when it asks for it.

## Using it

An application's app.mk:

    NORNNET_DIR = /path/to/nornNET          # optional
    MODULES    += $(NORNNET_DIR)/nornnet.mk
    NORNWIF_DIR = /path/to/nornWIF
    MODULES    += $(NORNWIF_DIR)/nornwif.mk

Its rtos_config.h needs RTOS_CFG_HEAP 1, RTOS_CFG_EVT_BITS 32, RTOS_CFG_EVGROUPS 1 and
two spare task ids at the top (NW_CFG_TASK_WIFI and NW_CFG_TASK_SERVICE, by default the
two highest). Then, as apps/nw02 does:

    nw_init(NW_HEAP_SPARE_START, NW_HEAP_SPARE_SIZE);   /* before rtos_start */
    ...
    nw_start();                                          /* in a task */
    nn_ifaceAdd(nw_netdev(), &iface, zero, zero, zero);
    nw_connect("ssid", "password", 20000);
    nn_dhcpStart(&dhcp, &iface, onDhcp, NULL);

That is all: from then on the station keeps the link up by itself (NW_CFG_RECONNECT:
straight back after a loss, then a delay doubling from 1 s to 60 s between attempts)
until nw_disconnect, and nornNET follows the link (nn_netdevLinkChanged): on a loss it
forgets the device's ARP entries, on a return the DHCP client confirms the lease, so an
outage on the same network keeps the address and the TCP connections; a new address
ends the connections bound to the old one with NN_ERR_DOWN. nw_connectStart is the
non-blocking form, nw_linkState and the events (nw_onEvent, run in the service task,
which must not call nornNET) tell what happens. apps/nw04 exercises it: three forced
drops recover in about 160 ms with DHCP confirmed within 100 ms and a TCP connection
intact; a vanished network is retried with the growing delay.

Build and run from this directory: `make nw01`, `make nw01-upload`, `make nw01-monitor`,
`make test-all`. The board must be on /dev/ttyUSB0 or PORT=... given. nw02 and nw04 read
the network from apps/nw02/nw_secrets.h, copied from nw_secrets.example.h and ignored by
git. `make nw04 EXTRA_CFLAGS=-DNW04_MANUAL=1` adds a two minute window to switch the
access point off and on for the real thing.

## Licences

Our code: the nornOS licence. lib/esp32: Espressif's binary libraries under Apache-2.0
(lib/LICENSE.*). include/: ESP-IDF headers, Apache-2.0. supplicant/: wpa_supplicant and
its ESP-IDF port, BSD (supplicant/COPYING).
