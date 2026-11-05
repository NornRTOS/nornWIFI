/**
 * @file nw04_reconnect.c
 * nw04: the link comes back by itself. The station joins the network of nw02's
 * nw_secrets.h and gets an address by DHCP; then the test drops the association three
 * times behind the policy's back (esp_wifi_disconnect at the library, the access point
 * sees a leave and the station a reason 8) and checks that the station reassociates on
 * its own, that DHCP confirms the lease, that the gateway answers pings again, and
 * that a TCP connection opened before the drop still works after it (same address).
 * Passes when every round recovers within its deadline.
 *
 * NW04_MANUAL 1: afterwards, waits two minutes for the access point to go away and
 * come back (switch the hotspot off and on): the same checks run on the real thing,
 * with the growing delay between attempts visible in the log. Then it disconnects.
 */

#include <string.h>
#include "rtos.h"
#include "uart.h"
#include "nw.h"
#include "nw_internal.h"
#include "esp_wifi.h"
#include "nn_net.h"
#include "nn_ip.h"
#include "nn_icmp.h"
#include "nn_dhcp.h"
#include "nn_tcp.h"
#include "../nw02/nw_secrets.h"

#ifndef NW04_MANUAL
# define NW04_MANUAL 0
#endif
#ifndef NW04_ROUNDS
# define NW04_ROUNDS 3
#endif
/* NW04_BACKOFF 1: after the rounds, the network is made to vanish from the station's
   point of view (its configuration is switched to a name nobody answers to, which the
   library treats as a loss): the attempts must come with a growing delay, and once
   the real name is back the station must return by itself. */
#ifndef NW04_BACKOFF
# define NW04_BACKOFF 1
#endif

#define TASK_TEST       1

static uint8_t _stackTest[6144] __attribute__((aligned(16)));
static nn_iface_t _if;
static nn_dhcp_t _dhcp;
static nn_tcp_t _tcp;
static uint8_t _ringR[2048], _ringT[1024], _page[1024];
static rtos_sem_t _dhcpSem = RTOS_SEM_INIT_BINARY(0);
static rtos_sem_t _upSem = RTOS_SEM_INIT_BINARY(0);
static rtos_sem_t _downSem = RTOS_SEM_INIT_BINARY(0);
static volatile uint32_t _replies;
static unsigned _failures;
static volatile uint32_t _tDown, _tUp;      /* ms, from the event hook */
static volatile uint32_t _downAt[16];        /* ms of each link down / failed attempt */
static volatile unsigned _downN;

static void say(const char *fmt, ...)
{
    char line[200];
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

static void expect(const char *what, bool ok)
{
    if(!ok)
    {
        ++ _failures;
        say("CHECK FAILED: %s", what);
    }
}

static void onDhcp(nn_dhcp_t *pD, bool bound, void *arg)
{
    (void)pD; (void)arg;
    say("nw04: dhcp %s %u.%u.%u.%u", bound ? "bound" : "lost", _if.addr.b[0], _if.addr.b[1], _if.addr.b[2], _if.addr.b[3]);
    if(bound)
        rtos_semGive(&_dhcpSem);
}

static void onReply(nn_ip4_t from, uint16_t id, uint16_t seq, const uint8_t *payload, uint16_t n, void *arg)
{
    (void)from; (void)id; (void)seq; (void)payload; (void)n; (void)arg;
    ++ _replies;
}

/* Only watching: the policy in nornWIF reconnects, nornNET's net task confirms the
   lease. nornNET must not be called from here (service task, above the lock ceiling). */
static void onEvent(int32_t id, const void *data, void *arg)
{
    (void)data; (void)arg;
    if(id == WIFI_EVENT_STA_CONNECTED)
    {
        _tUp = rtos_getTick();
        say("nw04: link up");
        rtos_semGive(&_upSem);
    }
    else if(id == WIFI_EVENT_STA_DISCONNECTED)
    {
        _tDown = rtos_getTick();
        if(_downN < 16) _downAt[_downN++] = _tDown;
        say("nw04: link down, reason %u, state %u", nw_lastDisconnectReason(), nw_linkState());
        rtos_semGive(&_downSem);
    }
}

/** \a n pings of the gateway 100 ms apart; the replies within 500 ms after the last. */
static uint32_t pingGateway(unsigned n)
{
    unsigned i;
    _replies = 0;
    for(i=0; i<n; ++i)
    {
        nn_icmpEchoRequest(_if.gw, 0x0404, (uint16_t)i, NULL, 32);
        rtos_delay(RTOS_MS(100));
    }
    rtos_delay(RTOS_MS(500));
    return _replies;
}

/** Waits for DHCP to be BOUND again after a link up (it rebinds to confirm). */
static bool waitBound(uint32_t timeoutMs)
{
    uint32_t t = 0;
    while(t < timeoutMs)
    {
        if(nn_dhcpState(&_dhcp) == NN_DHCP_BOUND)
            return true;
        rtos_delay(RTOS_MS(50));
        t += 50;
    }
    return nn_dhcpState(&_dhcp) == NN_DHCP_BOUND;
}

static bool hostAddr(nn_ip4_t *pDst, uint16_t *pPort)
{
    const char *s = NW_SECRET_HOST;
    unsigned v[4], i, k = 0, port = 80;
    if(s[0] == 0) return false;
    for(i=0; i<4; ++i)
    {
        unsigned n = 0, digits = 0;
        while(s[k] >= '0'  &&  s[k] <= '9') { n = n * 10 + (unsigned)(s[k++] - '0'); ++ digits; }
        if(digits == 0  ||  n > 255  ||  (i < 3  &&  s[k++] != '.')) return false;
        v[i] = n;
    }
    if(s[k] == ':') { port = 0; ++ k; while(s[k] >= '0'  &&  s[k] <= '9') port = port * 10 + (unsigned)(s[k++] - '0'); }
    *pDst = nn_ip4((uint8_t)v[0], (uint8_t)v[1], (uint8_t)v[2], (uint8_t)v[3]);
    *pPort = (uint16_t)port;
    return true;
}

static void waitClosed(void)
{
    unsigned j;
    for(j=0; j<500 && nn_tcpState(&_tcp) != NN_TCP_CLOSED; ++j)
        rtos_delay(RTOS_MS(10));
}

/** One recovery: the link is gone (by the test or the world); wait for it to be back,
    DHCP confirmed, pings answered. \a tcpOpen: a connection was open before, use it. */
static void checkRecovery(const char *what, bool tcpOpen, uint32_t upTimeoutMs)
{
    uint32_t replies;
    const bool up = rtos_semTake(&_upSem, RTOS_MS(upTimeoutMs));
    say("nw04: %s: reassociated %s after %lu ms, link state %u", what, up ? "yes" : "NO",
        up ? (unsigned long)(_tUp - _tDown) : 0ul, (unsigned)nw_linkState());
    expect("reassociated", up);
    if(!up) return;
    expect("dhcp confirmed", waitBound(6000));
    say("nw04: %s: dhcp %s, %u.%u.%u.%u, %lu ms after the link", what,
        nn_dhcpState(&_dhcp) == NN_DHCP_BOUND ? "bound" : "not bound", _if.addr.b[0], _if.addr.b[1], _if.addr.b[2], _if.addr.b[3],
        (unsigned long)(rtos_getTick() - _tUp));
    replies = pingGateway(5);
    say("nw04: %s: gateway pings %lu/5", what, (unsigned long)replies);
    expect("pings after recovery", replies >= 4);
    if(tcpOpen)
    {
        static const char req[] = "GET /tcp_echo.py HTTP/1.0\r\n\r\n";
        int32_t n, total = 0;
        const uint8_t st = nn_tcpState(&_tcp);
        if(st == NN_TCP_ESTABLISHED)
        {
            if(nn_tcpSend(&_tcp, req, (uint16_t)(sizeof(req) - 1), 2000) == (int32_t)(sizeof(req) - 1))
                while((n = nn_tcpRecv(&_tcp, _page, sizeof(_page), 5000)) > 0)
                    total += n;
            say("nw04: %s: tcp connection from before the drop: %ld bytes of page", what, (long)total);
            expect("tcp survived", total > 100);
        }
        else
            say("nw04: %s: tcp connection from before the drop is %s (err %d), address changed", what,
                nn_tcpStateName(st), (int)_tcp.err);
        nn_tcpClose(&_tcp);
        waitClosed();
    }
}

static void testTask(void *arg)
{
    esp_err_t err;
    nn_ip4_t host;
    uint16_t hostPort;
    const bool haveHost = hostAddr(&host, &hostPort);
    unsigned round;
    nw_stats_t st;
    (void)arg;

    say("nw04: join %s", NW_SECRET_SSID);
    nw_onEvent(onEvent, NULL);
    expect("nw_start", nw_start() == ESP_OK);
    nn_ifaceAdd(nw_netdev(), &_if, nn_ip4(0, 0, 0, 0), nn_ip4(0, 0, 0, 0), nn_ip4(0, 0, 0, 0));
    err = nw_connect(NW_SECRET_SSID, NW_SECRET_PASSWORD, 20000);
    say("nw04: connect -> 0x%x, rssi %d dBm", err, (int)nw_rssi());
    expect("connected", err == ESP_OK);
    (void)rtos_semTake(&_upSem, RTOS_NO_WAIT);
    if(err == ESP_OK)
    {
        expect("dhcpStart", nn_dhcpStart(&_dhcp, &_if, onDhcp, NULL) == NN_OK);
        expect("dhcp bound", rtos_semTake(&_dhcpSem, RTOS_MS(15000)));
        nn_icmpOnEchoReply(onReply, NULL);
        say("nw04: gateway pings %lu/5 before any drop", (unsigned long)pingGateway(5));

        for(round=1; round<=NW04_ROUNDS; ++round)
        {
            char what[16];
            bool tcpOpen = false;
            nw_snprintf(what, sizeof(what), "round %u", round);
            if(haveHost)
            {
                tcpOpen = nn_tcpOpen(&_tcp, _ringR, sizeof(_ringR), _ringT, sizeof(_ringT)) == NN_OK
                          &&  nn_tcpConnect(&_tcp, host, hostPort, 5000) == NN_OK;
                expect("tcp open before the drop", tcpOpen);
            }
            (void)rtos_semTake(&_downSem, RTOS_NO_WAIT);
            say("nw04: %s: dropping the association", what);
            (void)esp_wifi_disconnect();            /* below nw_disconnect: the policy stays armed */
            expect("link down seen", rtos_semTake(&_downSem, RTOS_MS(3000)));
            checkRecovery(what, tcpOpen, 15000);
        }

#if NW04_BACKOFF
        {
            wifi_config_t cfg;
            unsigned i, n0;
            memset(&cfg, 0, sizeof(cfg));
            strncpy((char *)cfg.sta.ssid, "nw04-no-such-network", sizeof(cfg.sta.ssid));
            strncpy((char *)cfg.sta.password, "nothing-here", sizeof(cfg.sta.password));
            cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
            say("nw04: backoff: the network vanishes (configuration switched to a name nobody has)");
            (void)rtos_semTake(&_downSem, RTOS_NO_WAIT);
            _downN = 0;
            expect("set bogus config", esp_wifi_set_config(WIFI_IF_STA, &cfg) == ESP_OK);
            (void)esp_wifi_disconnect();
            rtos_delay(RTOS_MS(16000));
            n0 = _downN;
            say("nw04: backoff: %u failed attempts in 16 s, gaps in ms:", n0);
            for(i=1; i<n0; ++i)
                say("nw04:   %lu", (unsigned long)(_downAt[i] - _downAt[i - 1]));
            expect("attempts kept coming", n0 >= 3);
            expect("delay grew", n0 >= 3  &&  _downAt[n0 - 1] - _downAt[n0 - 2] > _downAt[1] - _downAt[0]);
            expect("still trying", nw_linkState() == NW_LINK_CONNECTING);
            /* The real network again, through the API an application uses to change
               network while attempts are running: the policy continues with it. */
            say("nw04: backoff: the network is back (nw_connectStart with the real name)");
            (void)rtos_semTake(&_upSem, RTOS_NO_WAIT);
            _tDown = rtos_getTick();
            {
                const esp_err_t e = nw_connectStart(NW_SECRET_SSID, NW_SECRET_PASSWORD);
                if(e != ESP_OK) say("nw04: backoff: nw_connectStart -> 0x%x", e);
                expect("nw_connectStart", e == ESP_OK);
            }
            checkRecovery("backoff", false, 20000);
        }
#endif
#if NW04_MANUAL
        say("nw04: MANUAL: switch the access point off and on within two minutes");
        (void)rtos_semTake(&_downSem, RTOS_NO_WAIT);
        if(rtos_semTake(&_downSem, RTOS_MS(120000)))
            checkRecovery("manual", false, 180000);
        else
            say("nw04: MANUAL: nothing happened");
#endif
        nw_getStats(&st);
        say("nw04: link ups %lu downs %lu attempts %lu, rx %lu dropped %lu tx %lu errors %lu, heap free %lu min %lu",
            (unsigned long)st.linkUps, (unsigned long)st.linkDowns, (unsigned long)st.reconnectAttempts,
            (unsigned long)st.rxFrames, (unsigned long)st.rxDropped, (unsigned long)st.txFrames, (unsigned long)st.txErrors,
            (unsigned long)st.heapFree, (unsigned long)st.heapMinFree);
        nn_dhcpStop(&_dhcp);
        rtos_delay(RTOS_MS(200));
        (void)nw_disconnect();
        rtos_delay(RTOS_MS(300));
        expect("stays down after nw_disconnect", nw_linkState() == NW_LINK_DOWN  &&  !nw_isConnected());
    }
    nw_puts(_failures == 0 ? "RESULT nw04 PASS\r\n" : "RESULT nw04 FAIL\r\n");
    uart_flush();
    for(;;) rtos_delay(RTOS_MS(1000));
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
