/**
 * @file nw02_join.c
 * nw02: the station joins the network named in nw_secrets.h, gets an address by DHCP
 * through nornNET over the radio, pings the gateway, resolves a name and fetches a page
 * with TCP from the host named in NW_SECRET_HOST (optional). Passes when DHCP bound,
 * most pings came back and the TCP exchange worked.
 */

#include <string.h>
#include "rtos.h"
#include "uart.h"
#include "nw.h"
#include "nw_internal.h"
#include "nn_net.h"
#include "nn_ip.h"
#include "nn_icmp.h"
#include "nn_dhcp.h"
#include "nn_dns.h"
#include "nn_tcp.h"
#include "nw_secrets.h"
#include "esp_wifi.h"

#ifndef NW_SECRET_HOST
# define NW_SECRET_HOST     ""          /* "10.0.0.5:18080" serves tools/ with http.server */
#endif

#define TASK_TEST       1
#define PING_ID         0x4E57u
#define PINGS           40

static uint8_t _stackTest[6144] __attribute__((aligned(16)));
static nn_iface_t _if;
static nn_dhcp_t _dhcp;
static rtos_sem_t _dhcpSem = RTOS_SEM_INIT_BINARY(0);
static volatile uint32_t _replies = 0;
static uint32_t _rttSum = 0, _rttMax = 0, _sentAt[PINGS];
static volatile uint8_t _got[PINGS];
static uint32_t _rtt[PINGS];
static uint8_t _failures = 0;
static nn_tcp_t _tcp;
/* NW02_TPUT 1: after the HTTP GET, 200 KB to nornNET's tools/tcp_sink.py on the host
   (port NW02_SINK_PORT) and 200 KB back, throughput printed; larger rings for it. */
#ifndef NW02_TPUT
# define NW02_TPUT 0
#endif
#ifndef NW02_SINK_PORT
# define NW02_SINK_PORT 17010
#endif
#ifndef NW02_TPUT_KB
# define NW02_TPUT_KB 200
#endif
#ifndef NW02_RING
# define NW02_RING 8192
#endif
#if NW02_TPUT
static uint8_t _ringR[NW02_RING], _ringT[NW02_RING], _page[1400];
#else
static uint8_t _ringR[2048], _ringT[1024], _page[2048];
#endif

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

static void say(const char *fmt, ...)
{
    char line[128];
    va_list ap;
    va_start(ap, fmt);
    nw_vsnprintf(line, sizeof(line) - 2, fmt, ap);
    va_end(ap);
    strcat(line, "\r\n");
    nw_puts(line);
}

static void onDhcp(nn_dhcp_t *pD, bool bound, void *arg)
{
    (void)pD; (void)arg;
    if(bound)
        rtos_semGive(&_dhcpSem);
}

static void onReply(nn_ip4_t from, uint16_t id, uint16_t seq, const uint8_t *payload, uint16_t n, void *arg)
{
    (void)from; (void)payload; (void)n; (void)arg;
    if(id == PING_ID  &&  seq < PINGS)
    {
        const uint32_t rtt = rtos_getTimeUs() - _sentAt[seq];
        _rttSum += rtt;
        if(rtt > _rttMax) _rttMax = rtt;
        _got[seq] = 1;
        _rtt[seq] = rtt;
        ++ _replies;
    }
}

/* NW02_SNIFF 1: count every frame the radio hears while connected (promiscuous mode),
   by rate class, aggregation and channel width, to see which frames get through. */
#ifndef NW02_SNIFF
# define NW02_SNIFF 0
#endif
#ifndef NW02_STAY
# define NW02_STAY 0
#endif
/* NW02_RATES 1: before the normal ping series, ping the gateway with the data rate fixed
   to each of a list of rates (esp_wifi_config_80211_tx_rate) and report how many replies
   come back: a check of the transmitter per modulation. */
#ifndef NW02_RATES
# define NW02_RATES 0
#endif
#ifndef NW02_HEAP_MAIN
# define NW02_HEAP_MAIN 0
#endif
/* NW02_DUMP 1: while staying connected, print register ranges of the radio and the
   system twice (3 s and 5 s after the tests), to compare with ESP-IDF on the same board. */
#ifndef NW02_DUMP
# define NW02_DUMP 0
#endif
#if NW02_DUMP
static void dumpRange(uint32_t base, unsigned words)
{
    unsigned i;
    char line[120];
    for(i=0; i<words; i+=8)
    {
        unsigned k = (unsigned)nw_snprintf(line, sizeof(line), "D %08lx:", (unsigned long)(base + 4u * i));
        unsigned j;
        for(j=0; j<8 && i+j<words; ++j)
            k += (unsigned)nw_snprintf(line + k, sizeof(line) - k, " %08lx", (unsigned long)*(volatile uint32_t *)(base + 4u * (i + j)));
        nw_puts(line); nw_puts("\r\n");
    }
}
void nw_phyStageHook(int stage)
{
    char line[40];
    nw_snprintf(line, sizeof(line), "D phy stage %d\r\n", stage);
    nw_puts(line);
    dumpRange(0x3FF5CC00ul, 512);
    dumpRange(0x3FF45000ul, 64);
    dumpRange(0x3FF46000ul, 64);
    nw_puts("D end\r\n");
}
static void dumpAll(void)
{
    dumpRange(0x3FF00000ul, 64);     /* DPORT: clocks, resets, cache */
    dumpRange(0x3FF48000ul, 48);     /* RTC_CNTL */
    dumpRange(0x3FF48800ul, 24);     /* SENS */
    dumpRange(0x3FF5CC00ul, 512);    /* NRX + BB (baseband) */
    dumpRange(0x3FF73000ul, 1024);   /* WiFi MAC */
    dumpRange(0x3FF45000ul, 64);     /* FE2 */
    dumpRange(0x3FF46000ul, 64);     /* FE */
    nw_puts("D end\r\n");
}
#endif
/* A closed socket stays in use until its FIN exchange is over (nn_tcp.h); wait for that
   before the socket is reused. */
static void waitClosed(const char *what)
{
    unsigned j;
    for(j=0; j<500 && nn_tcpState(&_tcp) != NN_TCP_CLOSED; ++j)
        rtos_delay(RTOS_MS(10));
    expect(what, nn_tcpState(&_tcp) == NN_TCP_CLOSED);
}

#if NW02_STAY
/* NW02_STAY: the adapter's counters every 5 s, to tell a frame lost in the air from one
   dropped for want of a buffer (rxDropped) or failed on transmit (txErrors). */
static void sayStats(void)
{
    nw_stats_t st;
    nw_getStats(&st);
    say("nw02: stats rx %lu dropped %lu tx %lu errors %lu, heap free %lu min %lu",
        (unsigned long)st.rxFrames, (unsigned long)st.rxDropped, (unsigned long)st.txFrames, (unsigned long)st.txErrors,
        (unsigned long)st.heapFree, (unsigned long)st.heapMinFree);
}
#endif

#if NW02_SNIFF
static volatile uint32_t _snf[64][6];     /* [type/subtype][11b, 11g, 11n mpdu, 11n ampdu, cwb40, to us] */
static volatile uint32_t _snfTotal;
static volatile uint32_t _snfMcsToUs[8], _snfRateToUs[16];   /* frames to us by MCS (11n) / rate code (11bg) */
static volatile uint32_t _snfAck[2][16];
static volatile uint32_t _snfSgi, _snfLgi, _snfStbc, _snfAggN;   /* 11n frames to us: short/long GI, STBC, aggregated */
static volatile uint32_t _snfRtsToUs, _snfCtsToUs, _snfBarToUs, _snfCtrlOther, _snfSizeSum, _snfSizeN;
static volatile uint32_t _snfRetry, _snfDup, _snfDataToUs;    /* data frames to us: retry flag set, repeated sequence number */
static uint16_t _snfLastSeq = 0xFFFF;
/* RTS to us: how they arrive. A burst (another RTS within 3 ms) means the access point
   retried its RTS, i.e. it did not get our CTS; an RTS followed within 3 ms by a data
   frame to us means the exchange worked. */
static volatile uint32_t _snfRtsBurst, _snfRtsIsolated, _snfDataAfterRts, _snfDataAfterRtsMcs[8];
static uint32_t _snfLastRtsUs;                     /* ACK frames to us: [0]=11b/g by rate code, [1]=11n by mcs */
/* Timeline: the first frames addressed to us and everything nornNET sent. */
typedef struct { uint32_t tMs; uint8_t dir, sub, cls, idx, agg; int8_t rssi; } tl_t;
static tl_t _tl[48];
static volatile unsigned _tlN;
static uint32_t _tlT0;
static void tlAdd(uint8_t dir, uint8_t sub, uint8_t cls, uint8_t idx, uint8_t agg, int8_t rssi)
{
    if(_tlN < sizeof(_tl) / sizeof(_tl[0]))
    {
        tl_t *p = &_tl[_tlN++];
        p->tMs = (rtos_getTimeUs() - _tlT0) / 1000u; p->dir = dir; p->sub = sub; p->cls = cls; p->idx = idx; p->agg = agg; p->rssi = rssi;
    }
}
/* Gaps under load: a data frame to us more than 80 ms after the previous one (while the
   previous one was less than 1 s ago) is recorded with what the air looked like, and
   the delay from a data frame to us to nornNET's next transmission is tracked. */
typedef struct { uint32_t tMs, gapMs, sinceBeaconMs; uint8_t retry, beacons, mcs; } gap_t;
static gap_t _gap[12];
static volatile unsigned _gapN;
static uint32_t _gapLastRxUs, _gapLastBeaconUs;
static volatile unsigned _gapBeacons;
static uint8_t _apMac[6];
static volatile uint32_t _rxTxMaxUs, _rxTxSlow;
static volatile uint32_t _txPendUs, _txAckMaxUs, _txAckSlow, _txAckN;
static volatile uint32_t _snfBaToUs;  /* nornNET tx -> next ACK to us */
void nw02_txHook(uint16_t etherType)
{
    const uint32_t d = rtos_getTimeUs() - _gapLastRxUs;
    if(_txPendUs == 0) _txPendUs = rtos_getTimeUs() | 1u;
    if(_gapLastRxUs != 0  &&  d < 1000000u) { if(d > _rxTxMaxUs) _rxTxMaxUs = d; if(d > 20000u) ++ _rxTxSlow; }
    tlAdd(1, (uint8_t)(etherType >> 8), (uint8_t)etherType, 0, 0, 0);
}
static void sniffcb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    const wifi_promiscuous_pkt_t *p = buf;
    unsigned k;
    if(type == WIFI_PKT_MISC  ||  p->rx_ctrl.sig_len < 10) return;
    k = (p->payload[0] >> 2) & 63;
    if(type == WIFI_PKT_CTRL)
    {
        const bool toUs = memcmp(p->payload + 4, nw_mac(), 6) == 0;
        if((k == ((1u) | (13u << 2))  ||  k == ((1u) | (9u << 2)))  &&  toUs)   /* ACK or block ack to us */
        {
            if(_txPendUs != 0)
            {
                const uint32_t d = rtos_getTimeUs() - _txPendUs;
                ++ _txAckN; if(d > _txAckMaxUs) _txAckMaxUs = d; if(d > 20000u) ++ _txAckSlow;
                _txPendUs = 0;
            }
            if(k == ((1u) | (9u << 2))) ++ _snfBaToUs;
            else if(p->rx_ctrl.sig_mode != 0) ++ _snfAck[1][p->rx_ctrl.mcs & 15];
            else ++ _snfAck[0][p->rx_ctrl.rate & 15];
        }
        else if(k == ((1u) | (11u << 2))  &&  toUs)
        {
            const uint32_t now = rtos_getTimeUs();
            ++ _snfRtsToUs;
            if(now - _snfLastRtsUs < 3000u) ++ _snfRtsBurst; else ++ _snfRtsIsolated;
            _snfLastRtsUs = now;
        }
        else if(k == ((1u) | (12u << 2))  &&  toUs) ++ _snfCtsToUs;
        else if(k == ((1u) | (8u << 2))  &&  toUs) ++ _snfBarToUs;
        else ++ _snfCtrlOther;
        return;
    }
    ++ _snfTotal;
    if(p->rx_ctrl.sig_mode == 0) ++ _snf[k][p->rx_ctrl.rate < 4 ? 0 : 1];
    else ++ _snf[k][p->rx_ctrl.aggregation ? 3 : 2];
    if(p->rx_ctrl.cwb) ++ _snf[k][4];
    if(k == 32  &&  p->rx_ctrl.sig_len >= 16  &&  memcmp(p->payload + 10, _apMac, 6) == 0)
    {
        _gapLastBeaconUs = rtos_getTimeUs(); ++ _gapBeacons;
    }
    if(memcmp(p->payload + 4, nw_mac(), 6) == 0)
    {
        const uint8_t cls = p->rx_ctrl.sig_mode != 0 ? 2 : p->rx_ctrl.rate < 4 ? 0 : 1;
        const uint8_t idx = p->rx_ctrl.sig_mode != 0 ? (uint8_t)(p->rx_ctrl.mcs & 7) : (uint8_t)(p->rx_ctrl.rate & 15);
        ++ _snf[k][5];
        _snfSizeSum += p->rx_ctrl.sig_len; ++ _snfSizeN;
        if(type == WIFI_PKT_DATA  &&  p->rx_ctrl.sig_len >= 24)
        {
            const uint16_t seq = (uint16_t)((p->payload[22] | (p->payload[23] << 8)) >> 4);
            const uint32_t now = rtos_getTimeUs();
            ++ _snfDataToUs;
            if(now - _snfLastRtsUs < 3000u)
            {
                ++ _snfDataAfterRts;
                if(cls == 2) ++ _snfDataAfterRtsMcs[idx & 7];
                _snfLastRtsUs = now - 10000u;     /* count one data frame per RTS */
            }
            if(p->payload[1] & 0x08) ++ _snfRetry;
            if(seq == _snfLastSeq) ++ _snfDup;
            _snfLastSeq = seq;
            if(_apMac[0] == 0  &&  _apMac[1] == 0) memcpy(_apMac, p->payload + 10, 6);
            if(_gapLastRxUs != 0  &&  now - _gapLastRxUs > 80000u  &&  now - _gapLastRxUs < 1000000u
               &&  _gapN < sizeof(_gap) / sizeof(_gap[0]))
            {
                gap_t *g = &_gap[_gapN++];
                g->tMs = (now - _tlT0) / 1000u; g->gapMs = (now - _gapLastRxUs) / 1000u;
                g->sinceBeaconMs = (now - _gapLastBeaconUs) / 1000u; g->retry = (p->payload[1] & 0x08) != 0;
                g->beacons = (uint8_t)_gapBeacons; g->mcs = cls == 2 ? idx : 0xFF;
            }
            _gapLastRxUs = now; _gapBeacons = 0;
        }
        if(cls == 2)
        {
            ++ _snfMcsToUs[idx];
            if(p->rx_ctrl.sgi) ++ _snfSgi; else ++ _snfLgi;
            if(p->rx_ctrl.stbc) ++ _snfStbc;
            if(p->rx_ctrl.aggregation) ++ _snfAggN;
        }
        else ++ _snfRateToUs[idx];
        tlAdd(0, (uint8_t)k, cls, idx, (uint8_t)p->rx_ctrl.aggregation, (int8_t)p->rx_ctrl.rssi);
    }
}
static void sniffDump(void)
{
    unsigned i;
    char line[200];
    unsigned k = 0;
    for(i=0; i<8; ++i) k += (unsigned)nw_snprintf(line + k, sizeof(line) - k, " mcs%u=%lu", i, (unsigned long)_snfMcsToUs[i]);
    say("nw02: sniff: 11n frames to us by MCS:%s", line);
    k = 0;
    for(i=0; i<16; ++i) if(_snfRateToUs[i]) k += (unsigned)nw_snprintf(line + k, sizeof(line) - k, " r%u=%lu", i, (unsigned long)_snfRateToUs[i]);
    say("nw02: sniff: 11b/g frames to us by rate code:%s", line);
    say("nw02: timeline (ms since connect; tx=nornNET sent ethertype, rx=frame to us: type/subtype class b/g/n idx agg rssi):");
    for(i=0; i<_tlN; ++i)
    {
        const tl_t *p = &_tl[i];
        if(p->dir) say("  %5lu tx 0x%02x%02x", (unsigned long)p->tMs, p->sub, p->cls);
        else say("  %5lu rx %u/%-2u %c %u agg %u rssi %d", (unsigned long)p->tMs, p->sub & 3, p->sub >> 2, "bgn"[p->cls], p->idx, p->agg, p->rssi);
    }
    say("nw02: sniff: %lu frames while connected; per type/subtype: 11b 11g 11n-mpdu 11n-ampdu cwb40 to-us",
        (unsigned long)_snfTotal);
    for(i=0; i<64; ++i)
        if(_snf[i][0] + _snf[i][1] + _snf[i][2] + _snf[i][3] != 0)
            say("nw02: sniff: type %u subtype %2u: %lu %lu %lu %lu %lu %lu", i & 3, i >> 2, (unsigned long)_snf[i][0],
                (unsigned long)_snf[i][1], (unsigned long)_snf[i][2], (unsigned long)_snf[i][3], (unsigned long)_snf[i][4],
                (unsigned long)_snf[i][5]);
}
#endif

static unsigned parseUnsigned(const char *s)
{
    unsigned n = 0;
    while(*s >= '0'  &&  *s <= '9') n = n * 10 + (unsigned)(*s++ - '0');
    return n;
}

/** "a.b.c.d" -> four numbers; sscanf would pull newlib's reentrancy into the image. */
static bool parseIp4(const char *s, unsigned *a, unsigned *b, unsigned *c, unsigned *d)
{
    unsigned v[4], i, k = 0;
    for(i=0; i<4; ++i)
    {
        unsigned n = 0, digits = 0;
        while(s[k] >= '0'  &&  s[k] <= '9') { n = n * 10 + (unsigned)(s[k++] - '0'); ++ digits; }
        if(digits == 0  ||  n > 255  ||  (i < 3  &&  s[k++] != '.')) return false;
        v[i] = n;
    }
    *a = v[0]; *b = v[1]; *c = v[2]; *d = v[3];
    return true;
}

static void onEvent(int32_t id, const void *data, void *arg)
{
    (void)data; (void)arg;
    if(id == WIFI_EVENT_STA_CONNECTED)
        say("nw02: associated");
    else if(id == WIFI_EVENT_STA_DISCONNECTED)
        say("nw02: disconnected (reason %u)", nw_lastDisconnectReason());
}

static void testTask(void *arg)
{
    esp_err_t err;
    uint32_t i;
    nw_stats_t st;
    (void)arg;

    say("nw02: join %s", NW_SECRET_SSID);
    nw_onEvent(onEvent, NULL);
    expect("nw_start", nw_start() == ESP_OK);
#if NW02_DUMP
    rtos_delay(RTOS_MS(500));
    nw_puts("D after start\r\n");
    dumpAll();                                  /* calibrated, station started, not connected */
#endif
    nn_ifaceAdd(nw_netdev(), &_if, nn_ip4(0, 0, 0, 0), nn_ip4(0, 0, 0, 0), nn_ip4(0, 0, 0, 0));

    err = nw_connect(NW_SECRET_SSID, NW_SECRET_PASSWORD, 20000);
    say("nw02: connect -> 0x%x, rssi %d dBm", err, (int)nw_rssi());
    expect("connected", err == ESP_OK);
    if(err == ESP_OK)
    {
#if NW02_SNIFF
        _tlT0 = rtos_getTimeUs();
        {
            wifi_promiscuous_filter_t f = { .filter_mask = WIFI_PROMIS_FILTER_MASK_ALL };
            wifi_promiscuous_filter_t c = { .filter_mask = WIFI_PROMIS_CTRL_FILTER_MASK_ALL };
            (void)esp_wifi_set_promiscuous_filter(&f);
            (void)esp_wifi_set_promiscuous_ctrl_filter(&c);
        }
        (void)esp_wifi_set_promiscuous_rx_cb(sniffcb);
        (void)esp_wifi_set_promiscuous(true);
#endif
        expect("dhcpStart", nn_dhcpStart(&_dhcp, &_if, onDhcp, NULL) == NN_OK);
        expect("dhcp bound", rtos_semTake(&_dhcpSem, RTOS_MS(15000)));
        say("nw02: address %u.%u.%u.%u gateway %u.%u.%u.%u", _if.addr.b[0], _if.addr.b[1], _if.addr.b[2], _if.addr.b[3],
            _if.gw.b[0], _if.gw.b[1], _if.gw.b[2], _if.gw.b[3]);

        nn_icmpOnEchoReply(onReply, NULL);
#if NW02_RATES
        {
            static const struct { wifi_phy_rate_t rate; const char *name; } rates[] = {
                { WIFI_PHY_RATE_1M_L, "1M" }, { WIFI_PHY_RATE_2M_L, "2M" }, { WIFI_PHY_RATE_5M_L, "5.5M" },
                { WIFI_PHY_RATE_11M_L, "11M" }, { WIFI_PHY_RATE_6M, "6M" }, { WIFI_PHY_RATE_12M, "12M" },
                { WIFI_PHY_RATE_24M, "24M" }, { WIFI_PHY_RATE_54M, "54M" }, { WIFI_PHY_RATE_MCS0_LGI, "MCS0" },
                { WIFI_PHY_RATE_MCS1_LGI, "MCS1" }, { WIFI_PHY_RATE_MCS3_LGI, "MCS3" }, { WIFI_PHY_RATE_MCS5_LGI, "MCS5" },
                { WIFI_PHY_RATE_MCS7_LGI, "MCS7" } };
            unsigned r;
            for(r=0; r<sizeof(rates)/sizeof(rates[0]); ++r)
            {
                uint32_t before, j;
                esp_err_t e = esp_wifi_config_80211_tx_rate(WIFI_IF_STA, rates[r].rate);
                memset((void *)_got, 0, sizeof(_got));
                before = _replies;
                for(j=0; j<20; ++j)
                {
                    _sentAt[j] = rtos_getTimeUs();
                    nn_icmpEchoRequest(_if.gw, PING_ID, (uint16_t)j, NULL, 32);
                    rtos_delay(RTOS_MS(50));
                }
                rtos_delay(RTOS_MS(300));
#if NW02_SNIFF
                {
                    char acks[120];
                    unsigned k2 = 0, q;
                    for(q=0; q<16; ++q) if(_snfAck[0][q]) { k2 += (unsigned)nw_snprintf(acks + k2, sizeof(acks) - k2, " r%u=%lu", q, (unsigned long)_snfAck[0][q]); _snfAck[0][q] = 0; }
                    for(q=0; q<16; ++q) if(_snfAck[1][q]) { k2 += (unsigned)nw_snprintf(acks + k2, sizeof(acks) - k2, " mcs%u=%lu", q, (unsigned long)_snfAck[1][q]); _snfAck[1][q] = 0; }
                    say("nw02: tx rate %-5s (set 0x%x): %lu of 20 replies; ACKs heard:%s", rates[r].name, e, (unsigned long)(_replies - before), acks);
                }
#else
                say("nw02: tx rate %-5s (set 0x%x): %lu of 20 replies", rates[r].name, e, (unsigned long)(_replies - before));
#endif
            }
            (void)esp_wifi_config_80211_tx_rate(WIFI_IF_STA, WIFI_PHY_RATE_MAX);   /* back to rate control */
            _replies = 0; _rttSum = 0; _rttMax = 0;
            memset((void *)_got, 0, sizeof(_got));
        }
#endif
        for(i=0; i<PINGS; ++i)
        {
            _sentAt[i] = rtos_getTimeUs();
            nn_icmpEchoRequest(_if.gw, PING_ID, (uint16_t)i, NULL, 32);
            rtos_delay(RTOS_MS(100));
        }
        rtos_delay(RTOS_MS(500));
        say("nw02: ping gateway %lu/%u, rtt avg %lu us max %lu us", (unsigned long)_replies, PINGS,
            (unsigned long)(_replies ? _rttSum / _replies : 0), (unsigned long)_rttMax);
        if(_replies < PINGS)
        {
            char lost[64] = "";
            unsigned k = 0;
            for(i=0; i<PINGS && k<sizeof(lost)-4; ++i)
                if(!_got[i]) k += (unsigned)nw_snprintf(lost + k, sizeof(lost) - k, " %u", i);
            say("nw02: lost pings:%s", lost);
        }
        {
            char rtts[200] = "";
            unsigned k = 0;
            for(i=0; i<PINGS && k<sizeof(rtts)-6; ++i)
                k += (unsigned)nw_snprintf(rtts + k, sizeof(rtts) - k, " %lu", _got[i] ? (unsigned long)(_rtt[i] / 1000) : 0ul);
            say("nw02: rtt ms:%s", rtts);
        }
        expect("most pings answered", _replies >= PINGS - PINGS / 5);

        {
            nn_ip4_t a;
            nn_err_t r = nn_dnsResolve("www.google.com", &a, 5000);
            say("nw02: dns www.google.com -> %d (%u.%u.%u.%u)", (int)r, a.b[0], a.b[1], a.b[2], a.b[3]);
            expect("dns", r == NN_OK);
        }

        if(NW_SECRET_HOST[0] != 0)
        {
            /* HOST:PORT serving tools/ with python's http.server, as nornNET's tests use. */
            char host[48];
            const char *colon = strchr(NW_SECRET_HOST, ':');
            unsigned a0, a1, a2, a3, port = 80;
            strncpy(host, NW_SECRET_HOST, sizeof(host) - 1);
            host[sizeof(host) - 1] = 0;
            if(colon != NULL) { host[colon - NW_SECRET_HOST] = 0; port = parseUnsigned(colon + 1); }
            if(parseIp4(host, &a0, &a1, &a2, &a3))
            {
                const nn_ip4_t dst = nn_ip4((uint8_t)a0, (uint8_t)a1, (uint8_t)a2, (uint8_t)a3);
                int32_t n, total = 0;
                const uint32_t t0 = rtos_getTick();
                static const char req[] = "GET /tcp_echo.py HTTP/1.0\r\n\r\n";
                expect("tcpOpen", nn_tcpOpen(&_tcp, _ringR, sizeof(_ringR), _ringT, sizeof(_ringT)) == NN_OK);
                expect("tcpConnect", nn_tcpConnect(&_tcp, dst, (uint16_t)port, 5000) == NN_OK);
                expect("tcpSend", nn_tcpSend(&_tcp, req, (uint16_t)(sizeof(req) - 1), 2000) == (int32_t)(sizeof(req) - 1));
                while((n = nn_tcpRecv(&_tcp, _page, sizeof(_page), 5000)) > 0)
                    total += n;
                nn_tcpClose(&_tcp);
                say("nw02: http GET %ld bytes in %lu ms", (long)total, (unsigned long)(rtos_getTick() - t0));
                expect("http page", total > 100);
                waitClosed("httpClosed");
#if NW02_TPUT
                {
                    /* tcp_sink.py: 4 byte count, N pattern bytes up, 4 byte error count and
                       N pattern bytes down, then the server closes. */
                    const uint32_t nBytes = NW02_TPUT_KB * 1000ul;
                    uint32_t sent, got, bad = 0, k, t1, dt;
                    uint8_t hdr[4];
                    int32_t r;
                    nw_getStats(&st);
                    expect("sinkOpen", nn_tcpOpen(&_tcp, _ringR, sizeof(_ringR), _ringT, sizeof(_ringT)) == NN_OK);
                    expect("sinkConnect", nn_tcpConnect(&_tcp, dst, NW02_SINK_PORT, 5000) == NN_OK);
                    hdr[0] = (uint8_t)(nBytes >> 24); hdr[1] = (uint8_t)(nBytes >> 16); hdr[2] = (uint8_t)(nBytes >> 8); hdr[3] = (uint8_t)nBytes;
                    expect("sinkHdr", nn_tcpSend(&_tcp, hdr, 4, 1000) == 4);
                    t1 = rtos_getTick();
                    for(sent=0; sent<nBytes; )
                    {
                        const uint32_t n2 = nBytes - sent < sizeof(_page) ? nBytes - sent : (uint32_t)sizeof(_page);
                        for(k=0; k<n2; ++k) _page[k] = (uint8_t)(((sent + k) * 7u + 3u) & 255u);
                        r = nn_tcpSend(&_tcp, _page, n2, 10000);
                        if(r <= 0) break;
                        sent += (uint32_t)r;
                    }
                    dt = rtos_getTick() - t1;
                    expect("sinkSent", sent == nBytes);
                    say("nw02: tcp send %lu KB in %lu ms = %lu kB/s", (unsigned long)NW02_TPUT_KB, (unsigned long)dt, (unsigned long)(dt ? nBytes / dt : 0));
                    r = 0;
                    while(r < 4)
                    {
                        const int32_t m = nn_tcpRecv(&_tcp, hdr + r, 4 - (size_t)r, 10000);
                        if(m <= 0) break;
                        r += m;
                    }
                    expect("sinkReply", r == 4  &&  hdr[0] == 0  &&  hdr[1] == 0  &&  hdr[2] == 0  &&  hdr[3] == 0);
                    t1 = rtos_getTick();
                    for(got=0; got<nBytes; )
                    {
                        r = nn_tcpRecv(&_tcp, _page, sizeof(_page), 10000);
                        if(r <= 0) break;
                        for(k=0; k<(uint32_t)r; ++k)
                            if(_page[k] != (uint8_t)(((got + k) * 7u + 3u) & 255u)) ++bad;
                        got += (uint32_t)r;
                    }
                    dt = rtos_getTick() - t1;
                    expect("sinkGot", got == nBytes);
                    expect("sinkBad", bad == 0);
                    say("nw02: tcp receive %lu KB in %lu ms = %lu kB/s, %lu bad bytes", (unsigned long)NW02_TPUT_KB, (unsigned long)dt, (unsigned long)(dt ? nBytes / dt : 0), (unsigned long)bad);
                    expect("sinkEof", nn_tcpRecv(&_tcp, _page, sizeof(_page), 5000) == 0);
                    nn_tcpClose(&_tcp);
                    waitClosed("sinkClosed");
                    {
                        nw_stats_t s2;
                        nn_tcpStats_t ts;
                        nw_getStats(&s2);
                        nn_tcpGetStats(&ts);
                        say("nw02: during tcp: rx %lu dropped %lu tx %lu errors %lu, heap min %lu",
                            (unsigned long)(s2.rxFrames - st.rxFrames), (unsigned long)(s2.rxDropped - st.rxDropped),
                            (unsigned long)(s2.txFrames - st.txFrames), (unsigned long)(s2.txErrors - st.txErrors), (unsigned long)s2.heapMinFree);
                        say("nw02: tcp stats: rx %lu bad %lu ooo %lu oooDropped %lu dup %lu, tx %lu noBuf %lu rexmits %lu fast %lu timeouts %lu delAcks %lu",
                            (unsigned long)ts.rx, (unsigned long)(ts.rxBad + ts.rxBadChecksum), (unsigned long)ts.rxOoo, (unsigned long)ts.rxOooDropped, (unsigned long)ts.rxDup,
                            (unsigned long)ts.tx, (unsigned long)ts.txNoBuf, (unsigned long)ts.rexmits, (unsigned long)ts.fastRexmits, (unsigned long)ts.timeouts, (unsigned long)ts.delAcks);
                    }
                }
#endif
            }
        }
    }

#if NW02_SNIFF
    (void)esp_wifi_set_promiscuous(false);
    sniffDump();
#endif
    nw_osi_diagDump();
    nw_getStats(&st);
    say("nw02: rx %lu (group %lu) dropped %lu tx %lu errors %lu, events %lu timers %lu isrs %lu, heap free %lu min %lu",
        (unsigned long)st.rxFrames, (unsigned long)nw_netdev_rxGroup(), (unsigned long)st.rxDropped, (unsigned long)st.txFrames, (unsigned long)st.txErrors,
        (unsigned long)st.events, (unsigned long)st.timerCallbacks, (unsigned long)st.isrs,
        (unsigned long)st.heapFree, (unsigned long)st.heapMinFree);
#if NW02_STAY
    /* Stay connected for measurements from the host (ping the board's address). With
       NW02_SNIFF the frames to us are counted on, by MCS and rate, every 5 s. */
    nw_puts(_failures == 0 ? "RESULT nw02 PASS\r\n" : "RESULT nw02 FAIL\r\n");
    nw_puts("nw02: staying connected\r\n");
#if NW02_DUMP
    nw_puts("D after connect\r\n");
    rtos_delay(RTOS_MS(3000)); dumpAll();
    rtos_delay(RTOS_MS(2000)); dumpAll();
#endif
#if NW02_SNIFF
    (void)esp_wifi_set_promiscuous(true);
    for(;;)
    {
        char line[200];
        unsigned k = 0, j;
        rtos_delay(RTOS_MS(5000));
        sayStats();
        {
            unsigned g;
            say("nw02: rx->tx max %lu us, over 20 ms %lu; tx->ack %lu: max %lu us, over 20 ms %lu; gaps > 80 ms: %u",
                (unsigned long)_rxTxMaxUs, (unsigned long)_rxTxSlow, (unsigned long)_txAckN, (unsigned long)_txAckMaxUs, (unsigned long)_txAckSlow, _gapN);
            say("nw02:   block acks to us %lu", (unsigned long)_snfBaToUs); _snfBaToUs = 0;
            _txAckN = _txAckMaxUs = _txAckSlow = 0;
            for(g=0; g<_gapN; ++g)
                say("nw02:   at %lu ms gap %lu ms, %lu ms after the beacon, %u beacons in the gap, retry %u, mcs %u",
                    (unsigned long)_gap[g].tMs, (unsigned long)_gap[g].gapMs, (unsigned long)_gap[g].sinceBeaconMs, _gap[g].beacons, _gap[g].retry, _gap[g].mcs);
            _gapN = 0; _rxTxMaxUs = 0; _rxTxSlow = 0;
        }
        for(j=0; j<8; ++j) { k += (unsigned)nw_snprintf(line + k, sizeof(line) - k, " %lu", (unsigned long)_snfMcsToUs[j]); _snfMcsToUs[j] = 0; }
        k += (unsigned)nw_snprintf(line + k, sizeof(line) - k, " | 11b/g rate codes:");
        for(j=0; j<16; ++j) if(_snfRateToUs[j]) { k += (unsigned)nw_snprintf(line + k, sizeof(line) - k, " r%u=%lu", j, (unsigned long)_snfRateToUs[j]); _snfRateToUs[j] = 0; }
        {
            uint32_t n;
            (void)nw_osi_diagTickLate(&n);
            say("nw02: last 5 s, frames to us by MCS 0..7:%s | 11b/g%s", line, "");
            say("nw02:   data to us %lu: retry flag %lu, repeated seq %lu; rts %lu (bursts %lu, isolated %lu, data within 3 ms %lu: mcs0 %lu mcs1 %lu mcs2+ %lu) cts %lu bar %lu",
                (unsigned long)_snfDataToUs, (unsigned long)_snfRetry, (unsigned long)_snfDup,
                (unsigned long)_snfRtsToUs, (unsigned long)_snfRtsBurst, (unsigned long)_snfRtsIsolated, (unsigned long)_snfDataAfterRts,
                (unsigned long)_snfDataAfterRtsMcs[0], (unsigned long)_snfDataAfterRtsMcs[1],
                (unsigned long)(_snfDataAfterRtsMcs[2] + _snfDataAfterRtsMcs[3] + _snfDataAfterRtsMcs[4] + _snfDataAfterRtsMcs[5] + _snfDataAfterRtsMcs[6] + _snfDataAfterRtsMcs[7]),
                (unsigned long)_snfCtsToUs, (unsigned long)_snfBarToUs);
            _snfRtsBurst = _snfRtsIsolated = _snfDataAfterRts = 0; memset((void *)_snfDataAfterRtsMcs, 0, sizeof(_snfDataAfterRtsMcs));
            _snfSgi = _snfLgi = _snfStbc = _snfAggN = 0; _snfRtsToUs = _snfCtsToUs = _snfBarToUs = _snfCtrlOther = 0; _snfSizeSum = _snfSizeN = 0;
            _snfRetry = _snfDup = _snfDataToUs = 0;
        }
    }
#else
    for(;;) { rtos_delay(RTOS_MS(5000)); sayStats(); (void)esp_wifi_statis_dump(WIFI_STATIS_RXTX | WIFI_STATIS_HW); }
#endif
#endif
    /* Leave the access point cleanly so the next run does not meet a stale association. */
    (void)nw_disconnect();
    rtos_delay(RTOS_MS(200));
    nw_puts(_failures == 0 ? "RESULT nw02 PASS\r\n" : "RESULT nw02 FAIL\r\n");
    uart_flush();
    for(;;)
        rtos_delay(RTOS_MS(1000));
}

int main(void)
{
    uart_init(115200);
    rtos_init();
    nn_init();
#if NW02_HEAP_MAIN
    /* Experiment: the radio's heap (its DMA buffers) in the main SRAM bank instead of the
       second bank, as ESP-IDF has it. */
    {
        static uint8_t _heapMain[100 * 1024] __attribute__((aligned(16)));
        nw_init(_heapMain, sizeof(_heapMain));
    }
#else
    nw_init(NW_HEAP_SPARE_START, NW_HEAP_SPARE_SIZE);
#endif
    rtos_createTask(TASK_TEST, testTask, NULL, _stackTest, sizeof(_stackTest));
    rtos_start();
    return 0;
}
