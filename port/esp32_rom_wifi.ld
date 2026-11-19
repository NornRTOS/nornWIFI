/* ESP32 ROM functions the radio libraries call (addresses from Espressif's esp32.rom.ld,
   ESP-IDF v5.5.6). */
PROVIDE ( ets_delay_us = 0x40008534 );
PROVIDE ( roundup2 = 0x4000ab7c );
PROVIDE ( uart_div_modify = 0x400090cc );
PROVIDE ( phy_get_romfuncs = 0x40004100 );
PROVIDE ( esp_rom_crc32_le = 0x4005cfec );
PROVIDE ( esp_rom_delay_us = 0x40008534 );
PROVIDE ( g_ticks_per_us_pro = 0x3ffe01e0 );
