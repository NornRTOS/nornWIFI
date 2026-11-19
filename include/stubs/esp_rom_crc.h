/* The ESP32 ROM's CRC (port/esp32_rom_wifi.ld). */
#ifndef NW_STUB_ESP_ROM_CRC_INCLUDED
#define NW_STUB_ESP_ROM_CRC_INCLUDED
#include <stdint.h>
uint32_t esp_rom_crc32_le(uint32_t crc, const uint8_t *buf, uint32_t len);
#endif
