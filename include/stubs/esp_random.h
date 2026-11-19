/* ESP-IDF's random API over the nornWIF generator (nw_phy.c). */
#ifndef NW_STUB_ESP_RANDOM_INCLUDED
#define NW_STUB_ESP_RANDOM_INCLUDED
#include <stdint.h>
#include <stddef.h>
uint32_t esp_random(void);
void esp_fill_random(void *buf, size_t len);
#endif
