/* The ROM timer structure the radio libraries and the supplicant's event loop hand to
   the adapter's _timer_* functions (ESP-IDF esp32/rom/ets_sys.h); nw_osi.c keeps its
   own record behind timer_expire. */
#ifndef NW_STUB_ROM_ETS_SYS_INCLUDED
#define NW_STUB_ROM_ETS_SYS_INCLUDED
#include <stdint.h>
typedef void ETSTimerFunc(void *timer_arg);
typedef struct _ETSTIMER_
{
    struct _ETSTIMER_ *timer_next;
    uint32_t timer_expire;
    uint32_t timer_period;
    ETSTimerFunc *timer_func;
    void *timer_arg;
} ETSTimer;
#endif
