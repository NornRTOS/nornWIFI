/**
 * @file nw_cmd_wifi.c
 * `wifi` for nosh: the station's link on the console, a scan of the networks in range,
 * and joining or leaving a network at run time. Everything goes through nw.h; the scan
 * records (NW_CFG_CMD_SCAN_MAX of them, about 130 bytes each) are the one static buffer,
 * and it exists only when the command is linked.
 */

#include "nw_cmd.h"

#ifndef NW_CFG_CMD_SCAN_MAX
#define NW_CFG_CMD_SCAN_MAX     12u
#endif
#ifndef NW_CFG_CMD_SCAN_MS
#define NW_CFG_CMD_SCAN_MS      6000ul
#endif

static nw_cmd_credFunc_t _credFn = NULL;
static void *_credArg = NULL;
static wifi_ap_record_t _records[NW_CFG_CMD_SCAN_MAX];


void nw_cmd_setCredentials(nw_cmd_credFunc_t fn, void *arg)
{
    _credFn = fn;
    _credArg = arg;
}


static void putErr(const char *pWhatP, esp_err_t err)
{
    nosh_putsP(NOSH_STR("wifi: "));
    nosh_putsP(pWhatP);
    nosh_putsP(NOSH_STR(" failed, esp_err 0x"));
    nosh_putHex8((uint8_t)((uint32_t)err >> 8));
    nosh_putHex8((uint8_t)err);
    nosh_putEol();
}


static void putAuth(wifi_auth_mode_t mode)
{
    switch(mode)
    {
    case WIFI_AUTH_OPEN:            nosh_putsP(NOSH_STR("open")); break;
    case WIFI_AUTH_WEP:             nosh_putsP(NOSH_STR("wep")); break;
    case WIFI_AUTH_WPA_PSK:         nosh_putsP(NOSH_STR("wpa")); break;
    case WIFI_AUTH_WPA2_PSK:        nosh_putsP(NOSH_STR("wpa2")); break;
    case WIFI_AUTH_WPA_WPA2_PSK:    nosh_putsP(NOSH_STR("wpa/wpa2")); break;
    case WIFI_AUTH_WPA3_PSK:        nosh_putsP(NOSH_STR("wpa3")); break;
    case WIFI_AUTH_WPA2_WPA3_PSK:   nosh_putsP(NOSH_STR("wpa2/wpa3")); break;
    default:                        nosh_putsP(NOSH_STR("other")); break;
    }
}


static void cmdStatus(void)
{
    const uint8_t *mac = nw_mac();
    nw_stats_t st;
    uint8_t i;

    nosh_putsP(NOSH_STR("link     "));
    switch(nw_linkState())
    {
    case NW_LINK_UP:
        nosh_putsP(NOSH_STR("up, rssi "));
        nosh_putI32(nw_rssi());
        nosh_putsP(NOSH_STR(" dBm"));
        break;
    case NW_LINK_CONNECTING:
        nosh_putsP(NOSH_STR("connecting"));
        break;
    default:
        nosh_putsP(NOSH_STR("down"));
        break;
    }
    if(nw_lastDisconnectReason() != 0)
    {
        nosh_putsP(NOSH_STR(", last disconnect reason "));
        nosh_putU32(nw_lastDisconnectReason());
    }
    nosh_putEol();

    nosh_putsP(NOSH_STR("mac      "));
    for(i = 0; i < 6; ++i)
    {
        if(i > 0) nosh_putc(':');
        nosh_putHex8(mac[i]);
    }
    nosh_putEol();

    nw_getStats(&st);
    nosh_putsP(NOSH_STR("counters ")); nosh_putU32(st.linkUps);
    nosh_putsP(NOSH_STR(" ups, ")); nosh_putU32(st.linkDowns);
    nosh_putsP(NOSH_STR(" downs, ")); nosh_putU32(st.reconnectAttempts);
    nosh_putsP(NOSH_STR(" reconnect attempts; rx ")); nosh_putU32(st.rxFrames);
    nosh_putsP(NOSH_STR(" tx ")); nosh_putU32(st.txFrames);
    nosh_putsP(NOSH_STR(" frames, heap free ")); nosh_putU32(st.heapFree);
    nosh_putsP(NOSH_STR(" (min ")); nosh_putU32(st.heapMinFree); nosh_putc(')');
    nosh_putEol();
}


static void cmdScan(void)
{
    uint16_t n = NW_CFG_CMD_SCAN_MAX, i;
    esp_err_t err;

    nosh_putsP(NOSH_STR("scanning..."));
    nosh_putEol();
    err = nw_scan(_records, &n, NW_CFG_CMD_SCAN_MS);
    if(err != ESP_OK)
    {
        putErr(NOSH_STR("scan"), err);
        return;
    }
    nosh_putsP(NOSH_STR(" rssi ch  security   ssid"));
    nosh_putEol();
    for(i = 0; i < n; ++i)
    {
        const wifi_ap_record_t *pR = &_records[i];
        const char *s = (const char *)pR->ssid;
        uint8_t k = 0;
        nosh_putc(' ');
        nosh_putI32(pR->rssi);
        nosh_putc(' ');
        nosh_putU32Pad(pR->primary, 2);
        nosh_putsP(NOSH_STR("  "));
        /* security padded to 10 columns */
        {
            /* count what putAuth prints: simplest is to print into the column by type */
            switch(pR->authmode)
            {
            case WIFI_AUTH_OPEN:          k = 4; break;
            case WIFI_AUTH_WEP:           k = 3; break;
            case WIFI_AUTH_WPA_PSK:       k = 3; break;
            case WIFI_AUTH_WPA2_PSK:      k = 4; break;
            case WIFI_AUTH_WPA_WPA2_PSK:  k = 8; break;
            case WIFI_AUTH_WPA3_PSK:      k = 4; break;
            case WIFI_AUTH_WPA2_WPA3_PSK: k = 9; break;
            default:                      k = 5; break;
            }
            putAuth(pR->authmode);
            while(k++ < 11u)
                nosh_putc(' ');
        }
        if(s[0] == '\0')
            nosh_putsP(NOSH_STR("(hidden)"));
        else
            nosh_puts(s);
        nosh_putEol();
    }
    if(n == 0)
    {
        nosh_putsP(NOSH_STR("no networks"));
        nosh_putEol();
    }
    else if(n == NW_CFG_CMD_SCAN_MAX)
    {
        nosh_putsP(NOSH_STR("(list full: more may be in range)"));
        nosh_putEol();
    }
}


/** The psk is the rest of the line: tokens joined by one space, as nosh handed them. */
static void joinLine(char *pOut, size_t size, uint8_t argc, const char *const argv[], uint8_t from)
{
    size_t n = 0;
    uint8_t i;
    for(i = from; i < argc; ++i)
    {
        const char *p = argv[i];
        if(i > from  &&  n < size - 1u)
            pOut[n++] = ' ';
        while(*p != '\0'  &&  n < size - 1u)
            pOut[n++] = *p++;
    }
    pOut[n] = '\0';
}


static void cmdJoin(uint8_t argc, const char *const argv[])
{
    static char ssid[33], psk[65];
    esp_err_t err;

    if(argc >= 3)
    {
        joinLine(ssid, sizeof ssid, 3, argv, 2);            /* argv[2] alone */
        joinLine(psk, sizeof psk, argc, argv, 3);           /* the rest, may be empty */
    }
    else if(_credFn != NULL  &&  _credFn(ssid, sizeof ssid, psk, sizeof psk, _credArg)  &&  ssid[0] != '\0')
        ;
    else
    {
        nosh_putsP(NOSH_STR("wifi join <ssid> [psk]: no stored credentials to use"));
        nosh_putEol();
        return;
    }
    nosh_putsP(NOSH_STR("joining '"));
    nosh_puts(ssid);
    nosh_putsP(psk[0] != '\0' ? NOSH_STR("' (psk)...") : NOSH_STR("' (open)..."));
    nosh_putEol();
    err = nw_connectStart(ssid, psk[0] != '\0' ? psk : NULL);
    if(err != ESP_OK)
        putErr(NOSH_STR("join"), err);
    else
    {
        nosh_putsP(NOSH_STR("started; `wifi` shows the outcome"));
        nosh_putEol();
    }
}


static void cmdWifi(uint8_t argc, const char *const argv[])
{
    if(argc == 1)
        cmdStatus();
    else if(nosh_strEqP(argv[1], NOSH_STR("scan"))  &&  argc == 2)
        cmdScan();
    else if(nosh_strEqP(argv[1], NOSH_STR("join")))
        cmdJoin(argc, argv);
    else if(nosh_strEqP(argv[1], NOSH_STR("leave"))  &&  argc == 2)
    {
        const esp_err_t err = nw_disconnect();
        if(err != ESP_OK)
            putErr(NOSH_STR("leave"), err);
        else
        {
            nosh_putsP(NOSH_STR("left; reconnection off until the next join"));
            nosh_putEol();
        }
    }
    else
    {
        nosh_putsP(NOSH_STR("usage: wifi [scan | join [<ssid> [psk]] | leave]"));
        nosh_putEol();
    }
}

NOSH_CMD(nw_cmd_wifi, "wifi", "the link; scan, join, leave", cmdWifi);
