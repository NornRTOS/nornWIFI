/**
 * @file nw05_soak.c
 * nw05: the soak for the stability gate over the radio. Joins the network of nw02's
 * nw_secrets.h, gets an address by DHCP, then runs rounds forever: NW05_KB kilobytes to
 * nornNET's tools/tcp_sink.py on the host (NW_SECRET_HOST's address, port 17010) and
 * back with the pattern checked, five pings of the gateway, and one line of counters
 * every round (NW05_PERIOD_MS apart). A transfer that fails or stalls (10 s without
 * progress) is aborted, counted and the next round goes on; the host pings the board
 * meanwhile (tools/soak_wifi.sh). Never prints a RESULT line: the log is read at the
 * end for failures, link losses, heap and buffer pool drift, and the lease renewals.
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
#include "nn_tcp.h"
#include "nn_buf.h"
#include "../nw02/nw_secrets.h"

#ifndef NW05_KB
# define NW05_KB 200
#endif
#ifndef NW05_PERIOD_MS
# define NW05_PERIOD_MS 20000ul
#endif
#ifndef NW05_SINK_PORT
# define NW05_SINK_PORT 17010
#endif
#ifndef NW05_RING
# define NW05_RING 8192
#endif

#define TASK_TEST       1

static uint8_t _stackTest[6144] __attribute__((aligned(16)));
static nn_iface_t _if;
static nn_dhcp_t _dhcp;
static nn_tcp_t _tcp;
static uint8_t _ringR[NW05_RING], _ringT[NW05_RING], _page[1400];
static rtos_sem_t _dhcpSem = RTOS_SEM_INIT_BINARY(0);
static volatile uint32_t _replies;
static volatile uint32_t _linkEvents, _dhcpEvents;

static void say(const char *fmt, ...)
{
    char line[240];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = nw_vsnprintf(line, sizeof(line) - 2, fmt, ap);
    va_end(ap);
    if(n < 0) n = 0;
    if((size_t)n > sizeof(line) - 3) n = (int)sizeof(line) - 3;
    line[n] = '\r'; line[n + 1] = '\n'; line[n + 2] = 0;
    nw_puts(line);
}

static void onDhcp(nn_dhcp_t *pD, bool bound, void *arg)
{
    (void)pD; (void)arg;
    ++ _dhcpEvents;
    say("nw05: dhcp %s %u.%u.%u.%u", bound ? "bound" : "lost", _if.addr.b[0], _if.addr.b[1], _if.addr.b[2], _if.addr.b[3]);
    if(bound)
        rtos_semGive(&_dhcpSem);
}

static void onReply(nn_ip4_t from, uint16_t id, uint16_t seq, const uint8_t *payload, uint16_t n, void *arg)
{
    (void)from; (void)id; (void)seq; (void)payload; (void)n; (void)arg;
    ++ _replies;
}

static void onEvent(int32_t id, const void *data, void *arg)
{
    (void)data; (void)arg;
    if(id == WIFI_EVENT_STA_CONNECTED  ||  id == WIFI_EVENT_STA_DISCONNECTED)
    {
        ++ _linkEvents;
        say("nw05: link %s%s%u", id == WIFI_EVENT_STA_CONNECTED ? "up" : "down", id == WIFI_EVENT_STA_CONNECTED ? "" : ", reason ",
            id == WIFI_EVENT_STA_CONNECTED ? 0u : (unsigned)nw_lastDisconnectReason());
    }
}

static bool hostAddr(nn_ip4_t *pDst)
{
    const char *s = NW_SECRET_HOST;
    unsigned v[4], i, k = 0;
    if(s[0] == 0) return false;
    for(i=0; i<4; ++i)
    {
        unsigned n = 0, digits = 0;
        while(s[k] >= '0'  &&  s[k] <= '9') { n = n * 10 + (unsigned)(s[k++] - '0'); ++ digits; }
        if(digits == 0  ||  n > 255  ||  (i < 3  &&  s[k++] != '.')) return false;
        v[i] = n;
    }
    *pDst = nn_ip4((uint8_t)v[0], (uint8_t)v[1], (uint8_t)v[2], (uint8_t)v[3]);
    return true;
}

static uint32_t pingGateway(unsigned n)
{
    unsigned i;
    _replies = 0;
    for(i=0; i<n; ++i)
    {
        nn_icmpEchoRequest(_if.gw, 0x0505, (uint16_t)i, NULL, 32);
        rtos_delay(RTOS_MS(100));
    }
    rtos_delay(RTOS_MS(500));
    return _replies;
}

/** One transfer; returns 0 on success, else which step failed (1 connect, 2 header,
    3 send, 4 reply header, 5 receive, 6 pattern, 7 eof). Fills the rates. */
static unsigned transfer(nn_ip4_t host, uint32_t *pUpKBs, uint32_t *pDownKBs)
{
    const uint32_t nBytes = NW05_KB * 1000ul;
    uint32_t sent = 0, got = 0, bad = 0, k, t1, dt;
    uint8_t hdr[4];
    int32_t r;
    unsigned fail = 0;

    *pUpKBs = *pDownKBs = 0;
    if(nn_tcpOpen(&_tcp, _ringR, sizeof(_ringR), _ringT, sizeof(_ringT)) != NN_OK) return 1;
    if(nn_tcpConnect(&_tcp, host, NW05_SINK_PORT, 5000) != NN_OK) { nn_tcpAbort(&_tcp); return 1; }
    hdr[0] = (uint8_t)(nBytes >> 24); hdr[1] = (uint8_t)(nBytes >> 16); hdr[2] = (uint8_t)(nBytes >> 8); hdr[3] = (uint8_t)nBytes;
    if(nn_tcpSend(&_tcp, hdr, 4, 2000) != 4) fail = 2;
    t1 = rtos_getTick();
    while(fail == 0  &&  sent < nBytes)
    {
        const uint32_t n2 = nBytes - sent < sizeof(_page) ? nBytes - sent : (uint32_t)sizeof(_page);
        for(k=0; k<n2; ++k) _page[k] = (uint8_t)(((sent + k) * 7u + 3u) & 255u);
        r = nn_tcpSend(&_tcp, _page, n2, 10000);
        if(r <= 0) { fail = 3; break; }
        sent += (uint32_t)r;
    }
    dt = rtos_getTick() - t1;
    if(fail == 0) *pUpKBs = dt ? nBytes / dt : 0;
    r = 0;
    while(fail == 0  &&  r < 4)
    {
        const int32_t m = nn_tcpRecv(&_tcp, hdr + r, 4 - (size_t)r, 10000);
        if(m <= 0) { fail = 4; break; }
        r += m;
    }
    if(fail == 0  &&  (hdr[0] | hdr[1] | hdr[2] | hdr[3]) != 0) fail = 4;
    t1 = rtos_getTick();
    while(fail == 0  &&  got < nBytes)
    {
        r = nn_tcpRecv(&_tcp, _page, sizeof(_page), 10000);
        if(r <= 0) { fail = 5; break; }
        for(k=0; k<(uint32_t)r; ++k)
            if(_page[k] != (uint8_t)(((got + k) * 7u + 3u) & 255u)) ++bad;
        got += (uint32_t)r;
    }
    dt = rtos_getTick() - t1;
    if(fail == 0) *pDownKBs = dt ? nBytes / dt : 0;
    if(fail == 0  &&  bad != 0) fail = 6;
    if(fail == 0  &&  nn_tcpRecv(&_tcp, _page, sizeof(_page), 5000) != 0) fail = 7;
    if(fail == 0)
    {
        unsigned j;
        nn_tcpClose(&_tcp);
        for(j=0; j<500 && nn_tcpState(&_tcp) != NN_TCP_CLOSED; ++j) rtos_delay(RTOS_MS(10));
        if(nn_tcpState(&_tcp) != NN_TCP_CLOSED) nn_tcpAbort(&_tcp);
    }
    else
    {
        say("nw05: transfer failed at step %u: sent %lu got %lu bad %lu, tcp %s err %d", fail,
            (unsigned long)sent, (unsigned long)got, (unsigned long)bad, nn_tcpStateName(nn_tcpState(&_tcp)), (int)_tcp.err);
        nn_tcpAbort(&_tcp);
    }
    return fail;
}

static void testTask(void *arg)
{
    esp_err_t err;
    nn_ip4_t host;
    const bool haveHost = hostAddr(&host);
    uint32_t round = 0, fails = 0, pingLost = 0, pingSent = 0, upSum = 0, downSum = 0, okRounds = 0;
    uint32_t upMin = 0xFFFFFFFFul, downMin = 0xFFFFFFFFul;
    (void)arg;

    say("nw05: soak, join %s, %u KB each way every %lu ms", NW_SECRET_SSID, (unsigned)NW05_KB, (unsigned long)NW05_PERIOD_MS);
    nw_onEvent(onEvent, NULL);
    if(nw_start() != ESP_OK) { say("nw05: nw_start failed"); for(;;) rtos_delay(RTOS_MS(1000)); }
    nn_ifaceAdd(nw_netdev(), &_if, nn_ip4(0, 0, 0, 0), nn_ip4(0, 0, 0, 0), nn_ip4(0, 0, 0, 0));
    err = nw_connect(NW_SECRET_SSID, NW_SECRET_PASSWORD, 20000);
    say("nw05: connect -> 0x%x, rssi %d dBm", err, (int)nw_rssi());
    if(err != ESP_OK) { say("nw05: not connected, giving up"); for(;;) rtos_delay(RTOS_MS(1000)); }
    nn_dhcpStart(&_dhcp, &_if, onDhcp, NULL);
    rtos_semTake(&_dhcpSem, RTOS_MS(15000));
    nn_icmpOnEchoReply(onReply, NULL);
    say("nw05: address %u.%u.%u.%u gateway %u.%u.%u.%u, host %s", _if.addr.b[0], _if.addr.b[1], _if.addr.b[2], _if.addr.b[3],
        _if.gw.b[0], _if.gw.b[1], _if.gw.b[2], _if.gw.b[3], haveHost ? NW_SECRET_HOST : "none (pings only)");

    for(;;)
    {
        const uint32_t t0 = rtos_getTick();
        uint32_t up = 0, down = 0, replies;
        unsigned fail = 0;
        nw_stats_t st;
        nn_tcpStats_t ts;
        nn_bufStats_t bs;

        ++ round;
        if(haveHost  &&  nw_isConnected()  &&  nn_dhcpIsBound(&_dhcp))
        {
            fail = transfer(host, &up, &down);
            if(fail != 0) ++ fails;
            else { ++ okRounds; upSum += up; downSum += down; if(up < upMin) upMin = up; if(down < downMin) downMin = down; }
        }
        replies = pingGateway(5);
        pingSent += 5; pingLost += 5 - replies;
        nw_getStats(&st);
        nn_tcpGetStats(&ts);
        nn_lock(); nn_bufGetStats(&bs); nn_unlock();
        say("nw05: round %lu t %lu s: %s up %lu down %lu kB/s, ping %lu/5 | fails %lu lost %lu/%lu, ok %lu avg up %lu down %lu min up %lu down %lu "
            "| link ups %lu downs %lu attempts %lu, dhcp %u events %lu, rssi %d | rx %lu drop %lu tx %lu err %lu, heap %lu min %lu, bufs free %u/%u "
            "| tcp rexmit %lu fast %lu to %lu ooo %lu oooDrop %lu bad %lu",
            (unsigned long)round, (unsigned long)(rtos_getTick() / 1000ul), fail ? "FAIL" : "ok", (unsigned long)up, (unsigned long)down,
            (unsigned long)replies, (unsigned long)fails, (unsigned long)pingLost, (unsigned long)pingSent, (unsigned long)okRounds,
            (unsigned long)(okRounds ? upSum / okRounds : 0), (unsigned long)(okRounds ? downSum / okRounds : 0),
            (unsigned long)(okRounds ? upMin : 0), (unsigned long)(okRounds ? downMin : 0),
            (unsigned long)st.linkUps, (unsigned long)st.linkDowns, (unsigned long)st.reconnectAttempts, (unsigned)nn_dhcpState(&_dhcp),
            (unsigned long)_dhcpEvents, (int)nw_rssi(),
            (unsigned long)st.rxFrames, (unsigned long)st.rxDropped, (unsigned long)st.txFrames, (unsigned long)st.txErrors,
            (unsigned long)st.heapFree, (unsigned long)st.heapMinFree, (unsigned)bs.free, (unsigned)NN_CFG_NUM_BUFS,
            (unsigned long)ts.rexmits, (unsigned long)ts.fastRexmits, (unsigned long)ts.timeouts, (unsigned long)ts.rxOoo,
            (unsigned long)ts.rxOooDropped, (unsigned long)(ts.rxBad + ts.rxBadChecksum));
        {
            const uint32_t used = rtos_getTick() - t0;
            if(used < NW05_PERIOD_MS)
                rtos_delay(RTOS_MS(NW05_PERIOD_MS - used));
        }
    }
}

int main(void)
{
    uart_init(115200);
    rtos_init();
    nn_init();
    nw_init(NW_HEAP_SPARE_START, NW_HEAP_SPARE_SIZE);
    rtos_createTask(TASK_TEST, testTask, NULL, _stackTest, sizeof(_stackTest));
    rtos_start();
    return 0;
}
