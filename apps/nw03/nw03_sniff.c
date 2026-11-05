/**
 * @file nw03_sniff.c
 * Receiver check: count what the radio hears on one channel in promiscuous mode, per
 * second and per rate class, beacons per BSSID. Prints a table, no pass criterion
 * beyond "something was heard".
 */

#include <string.h>
#include "rtos.h"
#include "uart.h"
#include "nw.h"
#include "nw_internal.h"
#include "esp_wifi.h"

#ifndef NW03_CHANNEL
# define NW03_CHANNEL   10
#endif
#ifndef NW03_SECONDS
# define NW03_SECONDS   12
#endif
/** WIFI_SECOND_CHAN_BELOW for an HT40 access point with its secondary channel below. */
#ifndef NW03_SECOND
# define NW03_SECOND    WIFI_SECOND_CHAN_BELOW
#endif

#define TASK_TEST       1

static uint8_t _stackTest[6144] __attribute__((aligned(16)));
static volatile uint32_t _frames, _b, _g, _n, _beacons, _data, _mgmt, _ctrl, _rssiSum;
/* Frames per 802.11 type/subtype (fc byte 0 >> 2, 64 values) and rate class. */
static volatile uint32_t _sub[64][3];
static volatile uint32_t _mcs[8], _agg, _cwb40, _gRate[16];
static volatile int32_t _nfSum; static volatile int _nfMin = 0, _nfMax = -200;   /* all 11n frames by MCS, aggregated, 40 MHz; 11g by rate code */
static volatile uint32_t _perSec[NW03_SECONDS];
static volatile uint32_t _sec;
static uint8_t _bssid[6][6];
static volatile uint32_t _bssidBeacons[6];
static volatile int _bssidRssi[6];
static volatile uint8_t _bssidRate[6], _bssidMode[6];
static volatile unsigned _numBssid;

static void rxcb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    const wifi_promiscuous_pkt_t *p = buf;
    ++ _frames;
    if(_sec < NW03_SECONDS) ++ _perSec[_sec];
    _rssiSum += (uint32_t)(-p->rx_ctrl.rssi);
    _nfSum += p->rx_ctrl.noise_floor;
    if(p->rx_ctrl.noise_floor < _nfMin) _nfMin = p->rx_ctrl.noise_floor;
    if(p->rx_ctrl.noise_floor > _nfMax) _nfMax = p->rx_ctrl.noise_floor;
    {
        const unsigned cls = p->rx_ctrl.sig_mode != 0 ? 2 : p->rx_ctrl.rate < 4 ? 0 : 1;
        if(cls == 0) ++ _b; else if(cls == 1) ++ _g; else ++ _n;
        if(cls == 2) { ++ _mcs[p->rx_ctrl.mcs & 7]; if(p->rx_ctrl.aggregation) ++ _agg; if(p->rx_ctrl.cwb) ++ _cwb40; }
        else if(cls == 1) ++ _gRate[p->rx_ctrl.rate & 15];
        if(type != WIFI_PKT_MISC  &&  p->rx_ctrl.sig_len >= 2)
            ++ _sub[(p->payload[0] >> 2) & 63][cls];
    }
    if(type == WIFI_PKT_MGMT)
    {
        ++ _mgmt;
        if(p->rx_ctrl.sig_len >= 24  &&  p->payload[0] == 0x80)
        {
            unsigned i;
            ++ _beacons;
            for(i=0; i<_numBssid; ++i)
                if(memcmp(_bssid[i], p->payload + 16, 6) == 0) break;
            if(i == _numBssid  &&  _numBssid < 6)
            {
                memcpy(_bssid[i], p->payload + 16, 6);
                ++ _numBssid;
            }
            if(i < 6)
            {
                ++ _bssidBeacons[i];
                _bssidRssi[i] = p->rx_ctrl.rssi;
                _bssidRate[i] = (uint8_t)p->rx_ctrl.rate;
                _bssidMode[i] = (uint8_t)p->rx_ctrl.sig_mode;
            }
        }
    }
    else if(type == WIFI_PKT_DATA) ++ _data;
    else if(type == WIFI_PKT_CTRL) ++ _ctrl;
}

static void testTask(void *arg)
{
    char line[160];
    unsigned i, k;
    (void)arg;
    nw_puts("nw03: receiver check\r\n");
    if(nw_start() != ESP_OK)
    {
        nw_puts("RESULT nw03 FAIL\r\n");
        for(;;) rtos_delay(RTOS_MS(1000));
    }
    (void)esp_wifi_set_channel(NW03_CHANNEL, NW03_SECOND);
    (void)esp_wifi_set_promiscuous_rx_cb(rxcb);
    (void)esp_wifi_set_promiscuous(true);
    for(i=0; i<NW03_SECONDS; ++i)
    {
        if(i == NW03_SECONDS / 2)
        {
            /* Probe requests go out, the responses come back at the access points'
               lowest OFDM or 11b rate: the only way to make them talk to us here. */
            static wifi_ap_record_t recs[8];
            uint16_t n = 8;
            (void)nw_scan(recs, &n, 4000);
            nw_snprintf(line, sizeof(line), "nw03: scan in the middle saw %u access points\r\n", n);
            nw_puts(line);
            (void)esp_wifi_set_channel(NW03_CHANNEL, NW03_SECOND);
        }
        rtos_delay(RTOS_MS(1000));
        _sec = i + 1;
    }
    (void)esp_wifi_set_promiscuous(false);
    for(i=0; i<64; ++i)
        if(_sub[i][0] + _sub[i][1] + _sub[i][2] != 0)
        {
            nw_snprintf(line, sizeof(line), "nw03: type %u subtype %2u: 11b %lu 11g %lu 11n %lu\r\n", i & 3, i >> 2,
                        (unsigned long)_sub[i][0], (unsigned long)_sub[i][1], (unsigned long)_sub[i][2]);
            nw_puts(line);
        }
    nw_snprintf(line, sizeof(line), "nw03: channel %u, %lu frames in %u s: 11b %lu 11g %lu 11n %lu; mgmt %lu (beacons %lu) data %lu ctrl %lu, mean rssi -%lu\r\n",
                NW03_CHANNEL, (unsigned long)_frames, NW03_SECONDS, (unsigned long)_b, (unsigned long)_g, (unsigned long)_n,
                (unsigned long)_mgmt, (unsigned long)_beacons, (unsigned long)_data, (unsigned long)_ctrl,
                (unsigned long)(_frames ? _rssiSum / _frames : 0));
    nw_puts(line);
    k = (unsigned)nw_snprintf(line, sizeof(line), "nw03: frames per second:");
    for(i=0; i<NW03_SECONDS; ++i)
        k += (unsigned)nw_snprintf(line + k, sizeof(line) - k, " %lu", (unsigned long)_perSec[i]);
    nw_puts(line); nw_puts("\r\n");
    for(i=0; i<_numBssid; ++i)
    {
        nw_snprintf(line, sizeof(line), "nw03: bssid %02x:%02x:%02x:%02x:%02x:%02x beacons %lu (%lu.%lu/s) rssi %d rate %u mode %u\r\n",
                    _bssid[i][0], _bssid[i][1], _bssid[i][2], _bssid[i][3], _bssid[i][4], _bssid[i][5],
                    (unsigned long)_bssidBeacons[i], (unsigned long)(_bssidBeacons[i] / NW03_SECONDS),
                    (unsigned long)(_bssidBeacons[i] * 10 / NW03_SECONDS % 10), _bssidRssi[i], _bssidRate[i], _bssidMode[i]);
        nw_puts(line);
    }
    {
        extern uint32_t g_ticks_per_us_pro;
        extern void ets_delay_us(uint32_t us);
        const uint32_t c0 = rtos_port_ccount();
        ets_delay_us(1000);
        nw_snprintf(line, sizeof(line), "nw03: noise floor mean %ld min %d max %d dBm; rom ticks/us %lu, ets_delay_us(1000) took %lu us\r\n",
                    (long)(_frames ? _nfSum / (int32_t)_frames : 0), _nfMin, _nfMax, (unsigned long)g_ticks_per_us_pro,
                    (unsigned long)((rtos_port_ccount() - c0) / (F_CPU / 1000000ul)));
        nw_puts(line);
    }
    k = (unsigned)nw_snprintf(line, sizeof(line), "nw03: 11n by MCS:");
    for(i=0; i<8; ++i) k += (unsigned)nw_snprintf(line + k, sizeof(line) - k, " %lu", (unsigned long)_mcs[i]);
    k += (unsigned)nw_snprintf(line + k, sizeof(line) - k, ", aggregated %lu, 40 MHz %lu; 11g by rate code:", (unsigned long)_agg, (unsigned long)_cwb40);
    for(i=0; i<16; ++i) if(_gRate[i]) k += (unsigned)nw_snprintf(line + k, sizeof(line) - k, " r%u=%lu", i, (unsigned long)_gRate[i]);
    nw_puts(line); nw_puts("\r\n");
    nw_puts(_frames != 0 ? "RESULT nw03 PASS\r\n" : "RESULT nw03 FAIL\r\n");
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
