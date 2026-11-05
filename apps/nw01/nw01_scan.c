/**
 * @file nw01_scan.c
 * nw01: the ESP32 radio under nornOS. Starts the station, scans twice and prints the
 * access points found, then the counters. Passes when the station started, both scans
 * returned and at least one access point was seen.
 */

#include <string.h>
#include "rtos.h"
#include "uart.h"
#include "nw.h"
#include "nw_internal.h"
#include "esp_wifi.h"

#define TASK_TEST       1
#define RECORDS         16

static uint8_t _stackTest[4096] __attribute__((aligned(16)));
static wifi_ap_record_t _records[RECORDS];
static uint8_t _failures = 0;
static uint32_t _eventCount = 0;

static void expect(const char *what, bool ok)
{
    if(!ok)
    {
        ++ _failures;
        nw_puts("CHECK FAILED: ");
        nw_puts(what);
        nw_puts("\r\n");
    }
}

static void onEvent(int32_t id, const void *data, void *arg)
{
    (void)data; (void)arg;
    ++ _eventCount;
    nw_log(3, "nw01", "event %ld", (long)id);
}

static const char *authName(wifi_auth_mode_t m)
{
    switch(m)
    {
    case WIFI_AUTH_OPEN: return "open";
    case WIFI_AUTH_WEP: return "wep";
    case WIFI_AUTH_WPA_PSK: return "wpa";
    case WIFI_AUTH_WPA2_PSK: return "wpa2";
    case WIFI_AUTH_WPA_WPA2_PSK: return "wpa/wpa2";
    case WIFI_AUTH_WPA3_PSK: return "wpa3";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "wpa2/wpa3";
    default: return "other";
    }
}

static void testTask(void *arg)
{
    uint16_t n, round, total = 0;
    nw_stats_t st;
    char line[96];
    (void)arg;

    nw_puts("nw01: the ESP32 radio under nornOS\r\n");
    nw_onEvent(onEvent, NULL);
    expect("nw_start", nw_start() == ESP_OK);

    for(round=0; round<2; ++round)
    {
        uint16_t i;
        n = RECORDS;
        expect("scan", nw_scan(_records, &n, 8000) == ESP_OK);
        nw_snprintf(line, sizeof(line), "scan %u: %u access points\r\n", round + 1, n);
        nw_puts(line);
        for(i=0; i<n; ++i)
        {
            nw_snprintf(line, sizeof(line), "  %-32s ch %2u %4d dBm %s\r\n", (const char *)_records[i].ssid,
                        _records[i].primary, (int)_records[i].rssi, authName(_records[i].authmode));
            nw_puts(line);
        }
        total = (uint16_t)(total + n);
    }
    expect("at least one access point seen", total > 0);

    nw_getStats(&st);
    nw_snprintf(line, sizeof(line), "events %lu dropped %lu timers %lu isrs %lu heap free %lu min %lu\r\n",
                (unsigned long)st.events, (unsigned long)st.eventsDropped, (unsigned long)st.timerCallbacks,
                (unsigned long)st.isrs, (unsigned long)st.heapFree, (unsigned long)st.heapMinFree);
    nw_puts(line);
    expect("events delivered", _eventCount >= 3);
    expect("interrupts taken", st.isrs > 0);
    expect("no events dropped", st.eventsDropped == 0);

    nw_puts(_failures == 0 ? "RESULT nw01 PASS\r\n" : "RESULT nw01 FAIL\r\n");
    uart_flush();
    for(;;)
        rtos_delay(RTOS_MS(1000));
}

int main(void)
{
    uart_init(115200);
    rtos_init();
    nw_init(NW_HEAP_SPARE_START, NW_HEAP_SPARE_SIZE);
    rtos_createTask(TASK_TEST, testTask, NULL, _stackTest, sizeof(_stackTest));
    rtos_start();
    return 0;
}
