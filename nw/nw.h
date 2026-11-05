#ifndef NW_INCLUDED
#define NW_INCLUDED
/**
 * @file nw.h
 * nornWIF: the ESP32's WiFi radio under nornOS, without FreeRTOS or ESP-IDF, as a
 * station. Espressif's binary radio libraries run on an adapter over the nornOS
 * primitives (nw_osi.c); this is the small API an application uses. Everything below
 * except nw_init is called from a task after rtos_start.
 *
 * Errors are ESP-IDF's esp_err_t: 0 is ESP_OK, see esp_err.h and esp_wifi.h.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_wifi_types.h"
#include "nw_config.h"
#include "rtos.h"

/** Application callback for WiFi events: \a id is a wifi_event_t (WIFI_EVENT_STA_START,
    WIFI_EVENT_SCAN_DONE, WIFI_EVENT_STA_CONNECTED, WIFI_EVENT_STA_DISCONNECTED, ...)
    and \a data the matching wifi_event_* structure, or NULL. Runs in the service task;
    the WiFi API may be called from it. */
typedef void (*nw_eventFunc_t)(int32_t id, const void *data, void *arg);

/** Before rtos_start: creates the radio and service tasks (NW_CFG_TASK_WIFI and
    NW_CFG_TASK_SERVICE) and gives the kernel heap \a heapSize bytes at \a heap when it
    is not NULL. The radio needs about 60 KB of heap; NW_HEAP_SPARE_START is a region
    the image never uses. */
void nw_init(void *heap, size_t heapSize);

/** Power the radio, initialise the library and start station mode. The station MAC
    comes from the efuse; the nornNET device (NW_CFG_NORNNET) is registered. */
esp_err_t nw_start(void);

/** Scan all channels, blocking; fills up to *pCount records, strongest first, and sets
    *pCount to the number returned. */
esp_err_t nw_scan(wifi_ap_record_t *pRecords, uint16_t *pCount, uint32_t timeoutMs);

/** Connect to \a ssid; \a password NULL or empty for an open network. Blocks until
    associated (ESP_OK), refused (ESP_FAIL, see the disconnect reason in the event) or
    \a timeoutMs elapsed (ESP_ERR_TIMEOUT). After ESP_OK the station keeps the link up
    by itself (NW_CFG_RECONNECT) until nw_disconnect; after an error nothing more
    happens. */
esp_err_t nw_connect(const char *ssid, const char *password, uint32_t timeoutMs);

/** The same without waiting: the outcome arrives as WIFI_EVENT_STA_CONNECTED or
    WIFI_EVENT_STA_DISCONNECTED (nw_onEvent), and from here on the station reconnects
    by itself after every loss, at once the first time and then with a growing delay,
    until nw_disconnect. */
esp_err_t nw_connectStart(const char *ssid, const char *password);

/** Leave the network and stop reconnecting. */
esp_err_t nw_disconnect(void);

bool nw_isConnected(void);

/** nw_linkState: down and idle, between attempts or attempting, or associated. */
#define NW_LINK_DOWN        0u
#define NW_LINK_CONNECTING  1u
#define NW_LINK_UP          2u
uint8_t nw_linkState(void);

/** Switch the automatic reconnection on or off at run time (NW_CFG_RECONNECT is the
    start value); switching it on while the link is down starts an attempt. */
void nw_setReconnect(bool on);

/** The reason code of the last WIFI_EVENT_STA_DISCONNECTED (wifi_err_reason_t), 0 if none. */
uint8_t nw_lastDisconnectReason(void);

/** RSSI of the associated access point in dBm, 0 when not connected. */
int8_t nw_rssi(void);

/** The station MAC address, valid after nw_start. */
const uint8_t *nw_mac(void);

/** Register the application's event callback (one). */
void nw_onEvent(nw_eventFunc_t cb, void *arg);

/** Counters of the adapter and the device. */
typedef struct nw_stats
{
    uint32_t events;            /**< Events posted by the library. */
    uint32_t eventsDropped;     /**< Queue full. */
    uint32_t timerCallbacks;
    uint32_t isrs;              /**< Radio interrupts taken. */
    uint32_t rxFrames, rxDropped, txFrames, txErrors;   /**< nornNET device. */
    uint32_t heapFree, heapMinFree;
    uint32_t linkUps, linkDowns, reconnectAttempts;     /**< Associations, losses, attempts after a loss. */
} nw_stats_t;
void nw_getStats(nw_stats_t *pStats);

#if NW_CFG_NORNNET
struct nn_netdev;
/** The nornNET device of the station, registered by nw_start. Its link follows the
    association (nn_netdevLinkChanged): on a loss nornNET drops the device's ARP
    entries, on a return its DHCP client confirms the lease, or gets a new address,
    which ends the TCP connections bound to the old one with NN_ERR_DOWN. The
    application adds an interface and starts DHCP once, after nw_connect; nw04 shows
    it. The event callback runs in the service task, which is above the stack lock's
    ceiling: do not call nornNET from it. */
struct nn_netdev *nw_netdev(void);
#endif

/* Internal RAM of the ESP32 the nornOS image does not use: the second SRAM bank behind
   the ROM's data, free once the ROM loader has run. 113 KB, DMA capable. */
#define NW_HEAP_SPARE_START     ((void *)0x3FFE4350ul)
#define NW_HEAP_SPARE_SIZE      (0x40000000ul - 0x3FFE4350ul)

#endif  /* NW_INCLUDED */
