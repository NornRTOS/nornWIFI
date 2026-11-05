#ifndef NW_CONFIG_INCLUDED
#define NW_CONFIG_INCLUDED
/**
 * @file nw_config.h
 * Compile time configuration of nornWIF. An application overrides a value in its own
 * nw_config.h (nornwif.mk defines NW_CONFIG_H when the file exists next to app.mk).
 *
 * The kernel configuration of an application using nornWIF needs RTOS_CFG_HEAP 1,
 * RTOS_CFG_EVT_BITS 32 (the radio's event groups use up to 24 bits), RTOS_CFG_EVGROUPS 1
 * and RTOS_CFG_TIMER_TASK_PRIO 0 (timer callbacks in the tick, as the adapter expects).
 */

#ifdef NW_CONFIG_H
# include "nw_config.h"
#endif

/** nornOS task id (= priority) that runs the radio library's task. Give it the highest
    priority of the application, as ESP-IDF does, above the nornNET net task. */
#ifndef NW_CFG_TASK_WIFI
# ifdef RTOS_PRIO_WIFI_RADIO
#  define NW_CFG_TASK_WIFI      RTOS_PRIO_WIFI_RADIO   /* the nornCore profile's priority map */
# else
#  define NW_CFG_TASK_WIFI      RTOS_CFG_NUM_TASKS
# endif
#endif

/** Task id of the service task: runs the library's timer callbacks and delivers events
    to the application. Right below the radio task. */
#ifndef NW_CFG_TASK_SERVICE
# ifdef RTOS_PRIO_WIFI_SERVICE
#  define NW_CFG_TASK_SERVICE   RTOS_PRIO_WIFI_SERVICE
# else
#  define NW_CFG_TASK_SERVICE   (NW_CFG_TASK_WIFI - 1)
# endif
#endif

/** Stack of the radio task. ESP-IDF gives it 3584 bytes without WPA3; the supplicant
    and our logging run there too. */
#ifndef NW_CFG_WIFI_STACK
# define NW_CFG_WIFI_STACK      6144u
#endif

#ifndef NW_CFG_SERVICE_STACK
# define NW_CFG_SERVICE_STACK   3072u
#endif

/** Highest log level printed: 0 none, 1 error, 2 warning, 3 info, 4 debug, 5 verbose. */
#ifndef NW_CFG_LOG_LEVEL
# define NW_CFG_LOG_LEVEL       3
#endif

/** Events waiting for the service task, and the largest event payload kept. */
#ifndef NW_CFG_EVENT_QUEUE
# define NW_CFG_EVENT_QUEUE     8
#endif
#ifndef NW_CFG_EVENT_DATA_MAX
# define NW_CFG_EVENT_DATA_MAX  128
#endif

/** 1: the library's hot paths are in IRAM (set to 0 by nornwif.mk for NW_IRAM_OPT=0). */
#ifndef NW_CFG_IRAM_OPT
# define NW_CFG_IRAM_OPT        1
#endif

/** Radio buffer profile. The defaults are ESP-IDF's for the ESP32 (sdkconfig.h in
    include/stubs). NW_CFG_BUFFERS_SMALL 1 (make NW_SMALL=1) halves the heap the radio
    takes in exchange for fewer frames in flight and no AMPDU: static RX buffers are
    1.6 KB each and allocated at start, the dynamic ones on demand. Each value can also be
    set on its own. */
#ifndef NW_CFG_BUFFERS_SMALL
# define NW_CFG_BUFFERS_SMALL   0
#endif
#ifndef NW_CFG_RX_STATIC_BUFS
# define NW_CFG_RX_STATIC_BUFS  (NW_CFG_BUFFERS_SMALL ? 4 : CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM)
#endif
#ifndef NW_CFG_RX_DYNAMIC_BUFS
# define NW_CFG_RX_DYNAMIC_BUFS (NW_CFG_BUFFERS_SMALL ? 8 : CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM)
#endif
#ifndef NW_CFG_TX_DYNAMIC_BUFS
# define NW_CFG_TX_DYNAMIC_BUFS (NW_CFG_BUFFERS_SMALL ? 8 : CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM)
#endif
/** Management short buffers, 6..32. */
#ifndef NW_CFG_MGMT_SBUFS
# define NW_CFG_MGMT_SBUFS      (NW_CFG_BUFFERS_SMALL ? 8 : CONFIG_ESP_WIFI_MGMT_SBUF_NUM)
#endif
/** 1: AMPDU aggregation both ways (block ack window CONFIG_ESP_WIFI_RX_BA_WIN). */
#ifndef NW_CFG_AMPDU
# define NW_CFG_AMPDU           (NW_CFG_BUFFERS_SMALL ? 0 : 1)
#endif

/** Protocols the station offers: a mask of WIFI_PROTOCOL_11B, _11G, _11N (ESP-IDF's
    default is all three). 0 leaves the library's default. */
#ifndef NW_CFG_PROTOCOL
# define NW_CFG_PROTOCOL        0
#endif

/** 1: after the full RF calibration at boot, run the partial calibration with the data
    the full one produced, as ESP-IDF does on every boot after the first (it stores the
    data in NVS, nornWIF has no NVS). It leaves the RF front end registers FE+094,
    FE+0b4 and FE2+038 as IDF has them. It was a half measure against the receive
    defect that turned out to be a wrong PLL constant in nornOS (port_clock.c,
    2026-11-03); with that fixed the link runs at MCS 4 to 7 either way, so 0. */
/** Experiment: 1 hands the libraries ESP-IDF's feature_caps and sta_disconnected_pm
    instead of the stub sdkconfig's zero. No effect on the link (2026-11-03). */
#ifndef NW_CFG_IDF_CAPS
# define NW_CFG_IDF_CAPS        0
#endif

#ifndef NW_CFG_CAL_TWICE
# define NW_CFG_CAL_TWICE       0
#endif

/** Power save mode: WIFI_PS_NONE (no sleep, lowest latency) or WIFI_PS_MIN_MODEM, the
    library's own default. */
#ifndef NW_CFG_PS
# define NW_CFG_PS              WIFI_PS_NONE
#endif

/** Maximum transmit power in quarter dBm (8..78); 0 leaves the library's default (78). */
#ifndef NW_CFG_TX_POWER
# define NW_CFG_TX_POWER        0
#endif

/** Channel width of the station: WIFI_BW_HT20 or WIFI_BW_HT40. ESP-IDF defaults to
    HT40, but under nornOS the receiver hears only a fifth of the beacons of an HT40
    access point in that mode (doc/plan.md, 2026-11-03), so HT20 is the default until
    that is understood. */
#ifndef NW_CFG_BANDWIDTH
# define NW_CFG_BANDWIDTH       WIFI_BW_HT20
#endif

/** 1: offer protected management frames (802.11w, PMF) to the access point, as
    ESP-IDF does by default; 0: never. */
#ifndef NW_CFG_PMF
# define NW_CFG_PMF             1
#endif

/** Attempts nw_connect makes again within its timeout after the radio reports a
    disconnect while connecting (a lost auth or association response ends an attempt
    after a second). */
#ifndef NW_CFG_CONNECT_RETRIES
# define NW_CFG_CONNECT_RETRIES 5
#endif

/** 1: once associated (nw_connect returned ESP_OK, or nw_connectStart was called),
    the station reconnects by itself whenever the link is lost, until nw_disconnect:
    at once the first time, then with a delay doubling from NW_CFG_RECONNECT_MIN_MS to
    NW_CFG_RECONNECT_MAX_MS between attempts (each attempt still gets its
    NW_CFG_CONNECT_RETRIES). nw_setReconnect changes it at run time. */
#ifndef NW_CFG_RECONNECT
# define NW_CFG_RECONNECT       1
#endif
#ifndef NW_CFG_RECONNECT_MIN_MS
# define NW_CFG_RECONNECT_MIN_MS 1000ul
#endif
#ifndef NW_CFG_RECONNECT_MAX_MS
# define NW_CFG_RECONNECT_MAX_MS 60000ul
#endif

/** 1: build the nornNET network device (set by nornwif.mk when NORNNET_DIR is defined). */
#ifndef NW_CFG_NORNNET
# define NW_CFG_NORNNET         0
#endif

/** Name of the nornNET device. */
#ifndef NW_CFG_DEV_NAME
# define NW_CFG_DEV_NAME        "wl0"
#endif

#endif  /* NW_CONFIG_INCLUDED */
