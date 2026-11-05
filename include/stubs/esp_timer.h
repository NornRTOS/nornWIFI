/* ESP-IDF's monotonic clock over the adapter's 64 bit microsecond time. */
#ifndef NW_STUB_ESP_TIMER_INCLUDED
#define NW_STUB_ESP_TIMER_INCLUDED
#include <stdint.h>
int64_t esp_timer_get_time(void);
#endif
