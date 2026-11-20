/* ESP-IDF's ROM helpers: the delay is the ROM's, printing goes to the nornWIF log. */
#ifndef NW_STUB_ESP_ROM_SYS_INCLUDED
#define NW_STUB_ESP_ROM_SYS_INCLUDED
#include <stdint.h>
void esp_rom_delay_us(uint32_t us);
int esp_rom_printf(const char *fmt, ...);
#endif
