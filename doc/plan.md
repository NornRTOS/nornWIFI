# nornWIF: the ESP32 radio under nornOS

Phase 7 of the nornNET plan: the WiFi radio of the ESP32 driven by nornOS directly,
nothing of FreeRTOS or ESP-IDF in the image. Decided 2026-11-02 as a separate library
hung into a nornOS application through MODULES; started 2026-11-03.

## What the radio needs

Espressif ships the radio as binary libraries (components/esp_wifi/lib and esp_phy/lib
in ESP-IDF, Apache-2.0) and talks to the operating system only through a table of
function pointers, `wifi_osi_funcs_t` (esp_private/wifi_os_adapter.h, version 8 in
ESP-IDF v5.5.6). Measured on the v5.5.6 libraries for the ESP32:

| library | text (flash) | rodata | IRAM sections | data + bss |
|---|---|---|---|---|
| libnet80211 | 222 KB | 42 KB | 8 KB | 11 KB |
| libpp | 50 KB | 7 KB | 33 KB (sleep parts can go to flash) | 5 KB |
| libphy | 46 KB | 2.5 KB | 17 KB | 0.1 KB |
| librtc | 23 KB | 0.2 KB | | |
| libcore | 0.3 KB | | | |

The libraries reference 71 external symbols: libgcc arithmetic, a few libc string
functions, sprintf and puts, their own `*_printf` hooks, four ROM functions
(ets_delay_us, roundup2, uart_div_modify, phy_get_romfuncs), `esp_dport_access_reg_read`,
the regulatory tables, `hexstr2bin` from the supplicant, an ESP-NOW identifier and a few
mesh functions in objects that are never pulled in. Everything else goes through the
table: interrupts, critical sections, semaphores, mutexes, queues with front insertion,
event groups, one task, timers in task context, heap, events, NVS (optional), random
numbers, time, logging, the PHY and clock calls, and coexistence (stubbed, no Bluetooth).

The code does not fit the ESP32's 128 KB of IRAM or the 256 KB of executable RAM in
total, so nornOS had to learn to run code from flash first.

## Design

- **nornOS: code from flash** (done, nornOS tc09). FLASH_OBJS names objects and archives
  whose text and constants go to `.flash.text`/`.flash.rodata`, stored as a second
  image at FLASH_BASE and mapped by `rtos_port_flashInit` through the ROM's
  `cache_flash_mmu_set` into the data window 0x3F400000 and the instruction window
  0x400D0000 before main. Lessons: the ROM loader maps flash through the cache itself
  while loading an image larger than 64 KB, so the MMU is reset first; and the cache
  control register masks the 0x400D0000 window under the name IRAM0, which must be
  cleared like Espressif's bootloader does. A 4 KB table read through the cache costs 6
  cycles per word at 160 MHz.
- **The adapter** (nw_osi.c): one nornOS task for the library's "wifi" task, created
  before rtos_start and started when the library asks; a service task that runs the
  ETS timer callbacks (nornOS software timers queue them from the tick) and delivers
  events; queues as rings with a counting semaphore; mutexes as binary semaphores with
  owner and depth; event groups on nornOS event groups (32 bit events); a heap wrapper
  with a size header so that realloc works over rtos_malloc; interrupts through one
  trampoline that keeps the handler and argument per CPU interrupt and counts nesting
  for `_is_from_isr`.
- **Chip glue** (nw_phy.c): the power domain, clock gate and reset sequences of
  ESP-IDF's periph_ctrl.c and phy_init.c for the ESP32, `register_chipv7_phy` with the
  default init data and a full calibration at every boot (no NVS), the MAC from the
  efuse, the hardware random number register.
- **Supplicant**: ESP-IDF's wpa_supplicant port, built from source as a library with
  the defines ESP-IDF uses, for station WPA/WPA2 personal with PMF; WPS, WPA3, OWE,
  enterprise EAP and the soft AP are left out, the few hooks they would provide are
  stubbed.
- **nornNET device** (nw_netdev.c): Ethernet framed; the library copies on transmit,
  received frames are copied into a pool buffer in the library's task and queued for
  the net task; the link flag follows the association.
- **Memory**: the image's own DRAM holds the static data; the heap is the second SRAM
  bank (0x3FFE4350 up, 113 KB) that the nornOS image never uses. The radio takes about
  60 KB of it at start.

## Status

1. **Scan** (nw01, done 2026-11-03): PHY calibrated, station started with the efuse
   MAC, two scans listing the access points with the right channel, RSSI and
   authentication; 84 KB of heap free, no dropped events, radio interrupts counted.
2. **Join, DHCP, ping, DNS, HTTP** (nw02, verified 2026-11-03): first against a
   phone hotspot, then against the office access point (WPA2, channel 10, HT40,
   -60 dBm) with the host on the same network: association, the supplicant's 4-way
   handshake (WPA2-PSK, CCMP, PMF), DHCP, 40 pings at 100 ms, DNS, and an HTTP GET of
   929 bytes over nornNET TCP from the host (43 ms and 1025 ms in two runs). Three
   defects found on the way, all in the adapter's use of the radio, none in nornOS:
   - nw_connect gave up on the first failed attempt; it now tries again within its
     timeout (NW_CFG_CONNECT_RETRIES, 5).
   - The board was reset without leaving the access point, which then still held the
     old association; with PMF that costs an SA Query round (reason 4 after a second,
     "assoc -> assoc"). nw02 now disconnects at the end; after that the station
     associates on the first attempt in every run.
   - In HT40, ESP-IDF's default, the receiver hears a fifth of an HT40 access point's
     beacons (nw03: 1.3 to 2.3 per second against 8.5 to 8.9 in HT20). HT20 is the
     default (NW_CFG_BANDWIDTH); in HT40 half the runs lose the first 1.5 s of
     traffic after connecting.
   RESOLVED 2026-11-03 (late): the root cause was in nornOS, not in nornWIF. The PLL
   programming in nornOS port/xtensa/port_clock.c wrote BBADC_DSM 0x4c for the 320 MHz
   PLL where ESP-IDF 5.5.2 (clk_tree_ll.h) writes 0x84; that register tunes the
   baseband ADC's delta-sigma modulator, so every receive path above the lowest rates
   was lost while TX, ACKs and the register dumps of MAC, BB and FE all looked fine
   (the PLL's internal I2C registers are not memory mapped and were never compared).
   Found through the user's second question, random crashes at 240 MHz, which the
   same file caused with lref 1 instead of 0 for the 480 MHz PLL. With both constants
   corrected: the receiver decodes MCS 4 to 7, the access point runs at MCS 4 to 6
   towards the board, pings of 1400 bytes from the host lose 0 of 100 at 5 per second
   and 0 of 200 at 20 per second (NW02_STAY), nw02 40 of 40 and HTTP in 17 ms, and
   the second calibration is no longer needed (NW_CFG_CAL_TWICE default 0; with 0 the
   access point still reaches MCS 7). nornWIF 3 of 3, nornOS board suite 10 of 10 at
   160 and twice at 240 MHz.
   nornOS now defaults to 240 MHz (CPU_MHZ in arch.mk); nornWIF 3 of 3 there, host
   pings of 1400 bytes 0 of 100 lost at 5 per second and 198 to 200 of 200 at 20 per
   second, HTTP 14 ms. HT40 re-measured at 240 MHz: works now (0 of 200 lost) but the
   access point stays at MCS 2 to 4 where HT20 gets MCS 5 to 6, about the same air
   rate, so HT20 remains the default.
   Residual behaviour measured 2026-11-03 (late) with 1400 byte pings from the host at
   20 per second: about 1 in 600 to 1200 lost, and a few replies per 200 arrive after
   about 104 ms, two at once. Traced with NW02_STAY + NW02_SNIFF (per 5 s: adapter
   counters, gaps between frames to us, delay from frame to us to nornNET's reply,
   delay from the reply to the block ack, esp_wifi_statis_dump): the request arrives on
   time, the reply leaves nornNET within 3 ms, no buffer drops, no retry limits hit,
   but the block ack for the reply sometimes comes 50 ms later. With AMPDU off
   (NW_CFG_AMPDU=0) the 104 ms stalls vanish (0 of 600 over 50 ms) and the rare loss
   stays. MicroPython on the same board, power save off, shows the same 104 ms stalls
   (3 to 7 per 200) with 0 lost of 600, so the stalls are the access point's block ack
   reorder release (its reorder timeout is about 100 ms) after a subframe needed a
   retry on the shared channel 10 (DeltaOffice and others; the hardware counters show
   thousands of CCK/OFDM decode errors per second from other traffic). Not a nornWIF
   defect. AMPDU stays on (throughput); set NW_CFG_AMPDU=0 for latency-bound use.
   The defect as it was narrowed down on 2026-11-03 (evening), kept for the record:
   the receiver did not decode frames above MCS 1 (and not 11b 5.5/11 Mbit/s), so the access point's
   rate control settles at MCS 0 or 2 Mbit/s towards the board and large packets are
   lost under load (pings of 1400 bytes from the host at 1.1 Mbit/s lose 30 % to 80 %
   with round trips of seconds; MicroPython on the same board loses nothing at five
   times that). Established with the sniffing modes of nw02 (NW02_SNIFF, NW02_RATES,
   NW02_STAY, NW02_DUMP) and nw03:
   - transmitter fine: 20 of 20 pings answered at every fixed rate from 1 Mbit/s to
     MCS 7 (esp_wifi_config_80211_tx_rate);
   - ACKs and CTS fine: the access point's RTS arrive mostly isolated (not retried) and
     received frames are rarely duplicates, so the hardware's responses are heard;
   - radio health fine: noise floor -96 dBm at -61 dBm signal, ACK frames decoded at
     6 and 12 Mbit/s OFDM, MCS 0 with short and long guard interval and STBC decoded;
   - yet after 133 RTS from the access point only 20 data frames followed, all MCS 0:
     the frames at the rates it probes are not decoded.
   Ruled out: OS path (wake-up within 5 ms, tick late at most 0.4 ms, no drops), ISR
   latency and RX ring size (25 static buffers no better), DMA bank, PHY switching,
   AMPDU, PMF, CPU clock, transmit power, HT40, phy_init_data (identical to IDF's),
   calibration result, ROM delay (fixed), extra modem clocks (fixed), coex_bt_high_prio
   (real one now), phy_init_param_set(1) (added), temperature sensor power (added),
   ESP-IDF's rtc_init and peripheral clock setup (ported into nornOS port_clock.c).
   Register dumps compared with MicroPython (IDF 5.5.2, power save off) on the same
   board, taken after calibration, after start and after association (NW02_DUMP,
   scratch mpy_dump.py), showed: the baseband registers (NRX, BB) match once both run
   HT20; the RF front end differed in FE+094, FE+0b4 and FE2+038 already right after
   our full calibration. A partial calibration run after the full one, with the data
   the full one produced, which is what ESP-IDF does on every boot after the first,
   makes those registers identical to IDF's and halves the loss of large frames
   (NW_CFG_CAL_TWICE, now the default). The rate the access point reaches is still
   MCS 0 / 2 Mbit/s though. Also tried without effect: libpp and libnet80211 of
   IDF 5.5.2 (libphy and librtc are identical between 5.5.2 and 5.5.6), power save
   MIN_MODEM instead of none (NW_CFG_PS), IDF's feature_caps and sta_disconnected_pm
   (NW_CFG_IDF_CAPS, our stub sdkconfig gives caps 0). What still differs after
   association, apart from HT20/HT40 and counters: the MAC registers MAC+0e4
   (0x700f vs 0x7045), MAC+0f8/0fc (0xff800000 vs 0x05000000), MAC+d0c/d10 (set vs
   0), MAC+d60..d6c, the gain tables FE+004/008 (about +1 per entry, calibration
   noise), FE2+0dc/0f0, FE+0b8 bit 16, SENS TSENS power bits (IDF has them, the PHY
   clears ours), the RTC fast clock divider bit. Next lead: the MAC registers, which
   libpp writes from its configuration; candidates are the HT capability and
   aggregation settings it derives from the init configuration and the sdkconfig
   stub, and the RX filter / rate set the station announces.
   Note for experiments: a build with EXTRA_CFLAGS reuses build/<app>-esp32-extra,
   so clear it between variants.
3. **Memory knobs** (done 2026-11-03): measured on the board, nw02 unless noted.
   The question was how much IRAM and DRAM the stack leaves for an application; the
   answer is that IRAM is a placement question (code runs from flash), DRAM is the
   real limit.

   | Variant | IRAM | static DRAM | flash | heap free after start |
   |---|---|---|---|---|
   | as built first (everything of nornNET, libgcc, libc in IRAM) | 71.0 KB | 60.5 KB | 311 KB | 84.5 KB |
   | nornNET, libgcc, libc in flash (FLASH_OBJS in nw02/app.mk, now the default) | 47.2 KB | 60.5 KB | 335 KB | 84.5 KB |
   | + NW_IRAM_OPT=0: the libraries' .wifi0iram/.wifirxiram/.wifiextrairam in flash | 24.0 KB | 60.5 KB | 357 KB | 84.5 KB |
   | + NW_SMALL=1: rx 4 static 8 dynamic, tx 8, mgmt 8, no AMPDU | 24.0 KB | 60.5 KB | 357 KB | 94.4 KB |
   | nw01 (no nornNET), default | 51.7 KB | 35.9 KB | 310 KB | 84.5 KB |
   | nw01, NW_IRAM_OPT=0 NW_SMALL=1 | 28.5 KB | 35.9 KB | 332 KB | 94.4 KB |

   Scan passes with the hot code in flash (nw01, both scans list the same access
   points, no dropped events) and nw02 runs to the association attempt as before.
   The small profile saves 10 KB at start (the static RX buffers) and up to 40 KB more
   at peak, since the dynamic buffers are only allocated under traffic. What is still
   owed is the cost in throughput of the two knobs, which needs a network (nw02 with
   credentials). Of the 60 KB static DRAM, 23 KB is nornNET (12 buffers, net task
   stack), 19 KB the four task stacks, 12 KB the libraries' bss. The unused 119 KB of
   the main bank above bss is free for the application, and so is what is left in the
   radio heap.
4. **TCP throughput over wl0** (done 2026-11-03, 240 MHz, HT20, Tenforward at -65 to
   -77 dBm, host on Ethernet, nw02 with NW02_TPUT=1 against nornNET's tools/tcp_sink.py,
   pattern checked both ways, 0 bad bytes everywhere):

   | Variant (1 MB each way unless noted) | send | receive |
   |---|---|---|
   | default, 8 KB rings, 200 KB | 640-655 kB/s | 420-470 kB/s |
   | default, 8 KB rings, 1 MB, 11 runs | 413-631 kB/s | 309-615 kB/s |
   | 16 KB rings | 626-648 kB/s | 535-660 kB/s |
   | 4 KB rings | 393 kB/s | 318 kB/s |
   | NW_IRAM_OPT=0 (hot library code in flash), 200 KB | 314 kB/s | 313 kB/s |
   | NW_IRAM_OPT=0 NW_SMALL=1, 200 KB | 347 kB/s | 395 kB/s |
   | MicroPython 1.29 (lwIP, IDF 5.5.2, 160 MHz), same protocol, 1 MB | 197-451 kB/s | 265-315 kB/s |

   Receive is bounded by the ring (the advertised window): 8 KB over a 7 ms round
   trip. nornNET's statistics over 1 MB: 0 to 4 retransmissions, no out of order
   drops, no buffer shortage; the radio side 0 dropped, 0 errors, heap min 72 KB.
   The hot-code-in-flash knob costs about half the throughput (cache misses in the
   libraries' receive path), the small buffer profile costs little on top of it. Two
   things learnt: a socket must not be reused before nn_tcpState says CLOSED (nn_tcp.h
   says so; nw02 hung in the stack's list when it did), and one receive of 1 MB out of
   12 stalled until the 10 s timeout with no host state captured (the next 11 with the
   host's `ss -ti` sampled all passed), so that stall is an open item for the
   stability gate: rerun NW02_TPUT=1 NW02_TPUT_KB=1000 in a loop with the ss sampling
   and the tcp stats line until it recurs.
5. **Reconnection** (done 2026-11-03). nornWIF: after a successful nw_connect (or
   nw_connectStart, the non-blocking form) the station owns the link: a loss while
   associated is answered with an attempt at once, a failed attempt with the next one
   after a delay doubling from NW_CFG_RECONNECT_MIN_MS (1 s) to NW_CFG_RECONNECT_MAX_MS
   (60 s); each attempt keeps NW_CFG_CONNECT_RETRIES immediate retries for lost
   auth/assoc frames, but "no AP found" gets one only, since every such attempt is a
   full scan. nw_disconnect ends it, nw_setReconnect toggles it, nw_linkState reads it,
   nw_stats_t counts link ups, downs and attempts. nw_connectStart cancels an attempt
   in progress first (the library refuses a new configuration with ESP_ERR_WIFI_STATE
   otherwise). The timer runs in the service task (nw_osi_timerInit/Start/Stop, the
   adapter's own timer path). nornNET: a driver reports nn_netdevLinkChanged (from any
   task or ISR; it rides the receive queue as a marked pointer) and the net task acts
   under the lock: down flushes the device's ARP entries (nn_arpFlush), up calls
   nn_dhcpLinkUp for every client on the device (iface.pDhcp), which rebinds with a
   broadcast REQUEST, three tries 1.5 s apart, then starts over; the address is kept
   meanwhile, and nn_ifaceSetAddr tells TCP when it changes (nn_tcp_addrLost: the
   connections bound to the old address end with NN_ERR_DOWN, no RST). The reason it
   is in the net task: the service task's priority is above the stack lock's ceiling,
   so the first version, which flushed ARP from the event hook, hit the kernel's
   ceiling assertion (RTOS_ASSERT in rtos_mutexLock) and hung. Host tests: test_udp
   (link up confirm, unconfirmed restart) and test_tcp (address lost). Board: nw04
   drops the association three times behind the policy's back (esp_wifi_disconnect at
   the library): reassociated after 155 to 170 ms, DHCP confirmed 57 to 160 ms later
   (one REQUEST lost once: 1.6 s), pings answered, a TCP connection opened before the
   drop serves a GET after it; then the network "vanishes" (configuration switched to
   a name nobody has): attempts at 1, 2, 4, 8, 16, 32 s, each two scans of 2.4 s; and
   nw_connectStart with the real name brings it back in 170 ms. nornWIF 4 of 4,
   nornNET 7 of 7 (host, QEMU, board). Not yet done on the real thing: the access
   point going away and coming back (NW04_MANUAL=1 waits two minutes for it).
6. Later: power save (modem sleep is off), the soft AP, SNTP, and ESP-NOW if wanted. An open MAC layer in C after
   esp32-open-mac (replacing libnet80211 and libpp, libphy stays) was weighed on
   2026-11-03: weeks of work for about 20 KB IRAM and 25 KB DRAM, so only worth it
   for openness, and the adapter is already the seam for it (nornNET sees nw_netdev,
   the supplicant sees esp_wifi_driver.h, the PHY glue is nw_phy.c).
