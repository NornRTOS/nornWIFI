#ifndef NW_INTERNAL_INCLUDED
#define NW_INTERNAL_INCLUDED
/**
 * @file nw_internal.h
 * What the nornWIF sources share: the log, the heap wrapper, the chip glue (nw_phy.c),
 * the service task entry points (nw_osi.c) and the event hook (nw_wifi.c).
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdarg.h>
#include "nw_config.h"
#include "rtos.h"

/* --- nw_printf.c ------------------------------------------------------------------- */

/** Log a line "L (ms) tag: text" when \a level <= NW_CFG_LOG_LEVEL (1 error .. 5 verbose). */
void nw_log(int level, const char *tag, const char *fmt, ...);
void nw_logv(int level, const char *tag, const char *fmt, va_list ap);
/** As the libraries' own ESP_LOG macros expect: the text carries its prefix already. */
void nw_logRawv(int level, const char *fmt, va_list ap);
int nw_vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
int nw_snprintf(char *buf, size_t n, const char *fmt, ...);
/** Write a line without the log prefix; the UART is shared through a semaphore. */
void nw_puts(const char *s);

#define NW_LOGE(tag, ...)   nw_log(1, tag, __VA_ARGS__)
#define NW_LOGW(tag, ...)   nw_log(2, tag, __VA_ARGS__)
#define NW_LOGI(tag, ...)   nw_log(3, tag, __VA_ARGS__)
#define NW_LOGD(tag, ...)   nw_log(4, tag, __VA_ARGS__)

/* --- nw_osi.c ---------------------------------------------------------------------- */

/** Heap with a size header, so that realloc works over rtos_malloc. */
void *nw_malloc(size_t n);
void *nw_calloc(size_t count, size_t size);
void *nw_realloc(void *p, size_t n);
void nw_free(void *p);
uint32_t nw_heapFree(void);

/** True inside a radio interrupt handler. */
bool nw_inIsr(void);

/** Microseconds since start, 64 bits. */
int64_t nw_timeUs(void);

/** Called by nw_init: tasks, queues, semaphores. */
void nw_osi_init(void);
/** A one-shot timer whose callback runs in the service task, like the library's. */
typedef struct nwTimer
{
    rtos_timer_t t;
    void (*fn)(void *);
    void *arg;
    struct nwTimer *pNextPending;
    volatile uint8_t pending;
} nwTimer_t;
void nw_osi_timerInit(nwTimer_t *p, void (*fn)(void *), void *arg);
void nw_osi_timerStart(nwTimer_t *p, uint32_t ms);
void nw_osi_timerStop(nwTimer_t *p);

/** Statistics of the adapter. */
void nw_osi_getStats(uint32_t *pEvents, uint32_t *pEventsDropped, uint32_t *pTimerCallbacks, uint32_t *pIsrs);
/** Prints the interrupt to task diagnostics (warning level). */
void nw_osi_diagDump(void);
/** Longest lateness of the 1 ms tick since the last call (us) and how many were > 200 us. */
uint32_t nw_osi_diagTickLate(uint32_t *pCount);

/* --- nw_phy.c ---------------------------------------------------------------------- */

void nw_powerDomainOn(void);
void nw_phyEnable(void);
void nw_phyDisable(void);
void nw_phyCommonClockEnable(void);
void nw_phyCommonClockDisable(void);
void nw_wifiClockEnable(void);
void nw_wifiClockDisable(void);
void nw_wifiResetMac(void);
/** MAC address from the efuse: type 0 station, 1 soft AP, 2 BT, 3 Ethernet. */
int nw_readMac(uint8_t *mac, unsigned type);
uint32_t nw_random(void);

/* --- nw_wifi.c --------------------------------------------------------------------- */

/** Every event the library posts goes here first, in the service task, then to the
    application's callback. */
void nw_wifi_event(int32_t id, const void *data, size_t size);

/* --- nw_netdev.c ------------------------------------------------------------------- */

#if NW_CFG_NORNNET
void nw_netdev_register(const uint8_t mac[6]);
void nw_netdev_link(bool up);
void nw_netdev_getStats(uint32_t *pRx, uint32_t *pRxDropped, uint32_t *pTx, uint32_t *pTxErrors);
/** Frames received with a group (broadcast or multicast) destination, i.e. decrypted
    with the group key. */
uint32_t nw_netdev_rxGroup(void);
#endif

#endif  /* NW_INTERNAL_INCLUDED */
