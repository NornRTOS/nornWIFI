/**
 * @file nw_wifi.c
 * The station API of nornWIF over Espressif's esp_wifi: initialisation in the order of
 * ESP-IDF's wifi_init.c, the event hook that keeps the connection state and the
 * nornNET link, blocking scan and connect. The WPA supplicant (supplicant/) is started
 * after the library, as ESP-IDF does.
 */

#include <string.h>
#include "rtos.h"
#include "nw.h"
#include "nw_internal.h"
#include "esp_wpa.h"
#include "esp_wifi.h"
#include "esp_private/wifi.h"

#define TAG     "nw"

/* The event base symbol the library posts with (ESP_EVENT_DEFINE_BASE). */
esp_event_base_t const WIFI_EVENT = "WIFI_EVENT";

/* The vendor identifier of the ESP-NOW information element, Espressif's OUI; net80211
   references it although ESP-NOW is not linked. */
uint8_t g_espnow_user_oui[3] = { 0x18, 0xFE, 0x34 };

/* ESP-IDF's wifi_init.c wraps these two around the library's entry points. */
esp_err_t esp_wifi_connect_internal(void);
esp_err_t esp_wifi_disconnect_internal(void);
esp_err_t esp_wifi_connect(void) { return esp_wifi_connect_internal(); }
esp_err_t esp_wifi_disconnect(void) { return esp_wifi_disconnect_internal(); }


/*
 * Local data
 */

static rtos_sem_t _startSem = RTOS_SEM_INIT_BINARY(0);
static rtos_sem_t _scanSem = RTOS_SEM_INIT_BINARY(0);
static rtos_sem_t _connSem = RTOS_SEM_INIT_BINARY(0);
static rtos_sem_t _idleSem = RTOS_SEM_INIT_BINARY(0);   /* an attempt was cancelled and is over */
static volatile bool _started = false, _connected = false, _connecting = false;
static volatile uint8_t _lastReason = 0, _retriesLeft = 0;
static uint8_t _mac[6];
static nw_eventFunc_t _userCb = NULL;
static void *_userArg = NULL;
/* Reconnect policy: _want is the application's wish (nw_connect/nw_connectStart set it,
   nw_disconnect clears it), _blocking that nw_connect is waiting for the outcome of
   the current attempt and owns it. */
static volatile bool _want = false, _blocking = false, _auto = NW_CFG_RECONNECT != 0;
static volatile uint8_t _link = NW_LINK_DOWN;
static uint32_t _backoffMs = NW_CFG_RECONNECT_MIN_MS;
static nwTimer_t _reconnTimer;
static uint32_t _linkUps, _linkDowns, _reconnectAttempts;

static void scheduleReconnect(void);

/* An attempt, from the event hook or the timer (service task). */
static void attempt(void)
{
    if(!_want  ||  _connected  ||  _connecting)
        return;
    ++ _reconnectAttempts;
    _retriesLeft = NW_CFG_CONNECT_RETRIES;
    _connecting = true;
    _link = NW_LINK_CONNECTING;
    if(esp_wifi_connect() != ESP_OK)
    {
        _connecting = false;
        scheduleReconnect();
    }
    else
        NW_LOGI(TAG, "reconnecting");
}

static void reconnectFired(void *arg)
{
    (void)arg;
    attempt();
}

static void scheduleReconnect(void)
{
    if(!_want  ||  !_auto)
    {
        _link = NW_LINK_DOWN;
        return;
    }
    _link = NW_LINK_CONNECTING;
    NW_LOGI(TAG, "next attempt in %lu ms", (unsigned long)_backoffMs);
    nw_osi_timerStart(&_reconnTimer, _backoffMs);
    _backoffMs = _backoffMs * 2u < NW_CFG_RECONNECT_MAX_MS ? _backoffMs * 2u : NW_CFG_RECONNECT_MAX_MS;
}


/*
 * Events, in the service task
 */

void nw_wifi_event(int32_t id, const void *data, size_t size)
{
    (void)size;
    switch(id)
    {
    case WIFI_EVENT_STA_START:
        _started = true;
        rtos_semGive(&_startSem);
        break;
    case WIFI_EVENT_STA_STOP:
        _started = false;
        break;
    case WIFI_EVENT_SCAN_DONE:
        rtos_semGive(&_scanSem);
        break;
    case WIFI_EVENT_STA_CONNECTED:
    {
        const wifi_event_sta_connected_t *pE = data;
        _connected = true;
        _connecting = false;
        _link = NW_LINK_UP;
        _backoffMs = NW_CFG_RECONNECT_MIN_MS;
        ++ _linkUps;
        nw_osi_timerStop(&_reconnTimer);
        if(pE != NULL)
            NW_LOGI(TAG, "connected to %.*s channel %u", (int)pE->ssid_len, (const char *)pE->ssid, pE->channel);
#if NW_CFG_NORNNET
        nw_netdev_link(true);
#endif
        rtos_semGive(&_connSem);
        break;
    }
    case WIFI_EVENT_STA_DISCONNECTED:
    {
        const wifi_event_sta_disconnected_t *pE = data;
        const bool wasUp = _connected;
        _connected = false;
        if(pE != NULL)
        {
            _lastReason = pE->reason;
            NW_LOGI(TAG, "disconnected, reason %u rssi %d", pE->reason, (int)pE->rssi);
        }
        if(wasUp)
            ++ _linkDowns;
#if NW_CFG_NORNNET
        nw_netdev_link(false);
#endif
        if(_connecting)
        {
            /* An attempt can fail on a lost frame (auth or assoc response within a
               second); try again at once a few times, as ESP-IDF's examples do from
               their event handler. "No AP found" gets one such retry (a probe response
               can be lost too), after that the network is treated as gone: each attempt
               is a full scan, so the growing delay takes over. */
            const bool again = _retriesLeft > 0  &&  _want
                               &&  (pE == NULL  ||  pE->reason != WIFI_REASON_NO_AP_FOUND  ||  _retriesLeft == NW_CFG_CONNECT_RETRIES);
            if(again  &&  esp_wifi_connect() == ESP_OK)
            {
                -- _retriesLeft;
                NW_LOGI(TAG, "connecting again (%u left)", (unsigned)_retriesLeft);
            }
            else
            {
                _connecting = false;
                if(_blocking)
                    rtos_semGive(&_connSem);        /* nw_connect reports the failure */
                else
                    scheduleReconnect();
            }
        }
        else if(wasUp  &&  _want  &&  _auto)
        {
            /* The link was up and is gone (AP left, beacon timeout, deauth): straight
               back, the backoff only grows when that fails. */
            _backoffMs = NW_CFG_RECONNECT_MIN_MS;
            attempt();
        }
        else
        {
            _link = NW_LINK_DOWN;
            rtos_semGive(&_idleSem);
        }
        break;
    }
    default:
        NW_LOGD(TAG, "event %ld", (long)id);
        break;
    }
    if(_userCb != NULL)
        _userCb(id, data, _userArg);
}

void nw_onEvent(nw_eventFunc_t cb, void *arg)
{
    _userCb = cb;
    _userArg = arg;
}


/*
 * API
 */

void nw_init(void *heap, size_t heapSize)
{
    if(heap != NULL  &&  heapSize != 0)
        rtos_heapInit(heap, heapSize);
    nw_osi_init();
    nw_osi_timerInit(&_reconnTimer, reconnectFired, NULL);
}

esp_err_t nw_start(void)
{
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err;

    cfg.nvs_enable = 0;
#if NW_CFG_IDF_CAPS
    /* Experiment: the feature capabilities and disconnected power management ESP-IDF's
       default configuration gives the library (WPA3-SAE, GMAC, BSS max idle, OWE). */
    cfg.feature_caps = (1u << 0) | (1u << 5) | (1u << 8) | (1u << 10);
    cfg.sta_disconnected_pm = 1;
#endif
    cfg.wifi_task_stack_size = NW_CFG_WIFI_STACK;
    cfg.static_rx_buf_num = NW_CFG_RX_STATIC_BUFS;
    cfg.dynamic_rx_buf_num = NW_CFG_RX_DYNAMIC_BUFS;
    cfg.dynamic_tx_buf_num = NW_CFG_TX_DYNAMIC_BUFS;
    cfg.mgmt_sbuf_num = NW_CFG_MGMT_SBUFS;
    cfg.ampdu_rx_enable = NW_CFG_AMPDU;
    cfg.ampdu_tx_enable = NW_CFG_AMPDU;
    cfg.rx_ba_win = NW_CFG_AMPDU ? CONFIG_ESP_WIFI_RX_BA_WIN : 0;
    NW_LOGI(TAG, "init config: tx type %d static %d cache %d, csi %d, amsdu %d, nvs %d, nano %d, ba win %d, core %d, beacon max %d, "
                 "caps 0x%lx, sta_disc_pm %d, espnow enc %d, hetb %d, hesigb %d, mgmt buf type %d num %d, magic 0x%x",
            cfg.tx_buf_type, cfg.static_tx_buf_num, cfg.cache_tx_buf_num, cfg.csi_enable, cfg.amsdu_tx_enable, cfg.nvs_enable,
            cfg.nano_enable, cfg.rx_ba_win, cfg.wifi_task_core_id, cfg.beacon_max_len, (unsigned long)cfg.feature_caps,
            cfg.sta_disconnected_pm, cfg.espnow_max_encrypt_num, cfg.tx_hetb_queue_num, cfg.dump_hesigb_enable,
            cfg.rx_mgmt_buf_type, cfg.rx_mgmt_buf_num, (unsigned)cfg.magic);
    NW_LOGI(TAG, "buffers: rx %d static %d dynamic, tx %d dynamic, mgmt %d, ampdu %d, hot code in %s",
            cfg.static_rx_buf_num, cfg.dynamic_rx_buf_num, cfg.dynamic_tx_buf_num, cfg.mgmt_sbuf_num,
            cfg.ampdu_rx_enable, NW_CFG_IRAM_OPT ? "iram" : "flash");
    nw_powerDomainOn();
    err = esp_wifi_init_internal(&cfg);
    if(err != ESP_OK)
    {
        NW_LOGE(TAG, "esp_wifi_init_internal: 0x%x", err);
        return err;
    }
    esp_wifi_internal_set_log_level(NW_CFG_LOG_LEVEL >= 5 ? WIFI_LOG_VERBOSE : NW_CFG_LOG_LEVEL >= 4 ? WIFI_LOG_DEBUG
                                    : NW_CFG_LOG_LEVEL >= 3 ? WIFI_LOG_INFO : NW_CFG_LOG_LEVEL >= 2 ? WIFI_LOG_WARNING
                                    : NW_CFG_LOG_LEVEL >= 1 ? WIFI_LOG_ERROR : WIFI_LOG_NONE);
    err = esp_supplicant_init();
    if(err != ESP_OK)
    {
        NW_LOGE(TAG, "esp_supplicant_init: 0x%x", err);
        return err;
    }
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if(err != ESP_OK)
    {
        NW_LOGE(TAG, "esp_wifi_set_mode: 0x%x", err);
        return err;
    }
    (void)esp_wifi_set_ps(NW_CFG_PS);
    if(NW_CFG_PROTOCOL != 0)
    {
        err = esp_wifi_set_protocol(WIFI_IF_STA, NW_CFG_PROTOCOL);
        if(err != ESP_OK)
            NW_LOGW(TAG, "esp_wifi_set_protocol: 0x%x", err);
    }
    err = esp_wifi_set_bandwidth(WIFI_IF_STA, NW_CFG_BANDWIDTH);
    if(err != ESP_OK)
        NW_LOGW(TAG, "esp_wifi_set_bandwidth: 0x%x", err);
    err = esp_wifi_get_mac(WIFI_IF_STA, _mac);
    if(err != ESP_OK)
        nw_readMac(_mac, 0);
#if NW_CFG_NORNNET
    nw_netdev_register(_mac);
#endif
    err = esp_wifi_start();
    if(err == ESP_OK  &&  NW_CFG_TX_POWER != 0)
        (void)esp_wifi_set_max_tx_power(NW_CFG_TX_POWER);
    if(err != ESP_OK)
    {
        NW_LOGE(TAG, "esp_wifi_start: 0x%x", err);
        return err;
    }
    if(!rtos_semTake(&_startSem, RTOS_MS(5000)))
        NW_LOGW(TAG, "no STA_START event");
    NW_LOGI(TAG, "station %02x:%02x:%02x:%02x:%02x:%02x started, %lu bytes of heap free",
            _mac[0], _mac[1], _mac[2], _mac[3], _mac[4], _mac[5], (unsigned long)nw_heapFree());
    return ESP_OK;
}

esp_err_t nw_scan(wifi_ap_record_t *pRecords, uint16_t *pCount, uint32_t timeoutMs)
{
    esp_err_t err;
    uint16_t n = 0;

    (void)rtos_semTake(&_scanSem, RTOS_NO_WAIT);
    err = esp_wifi_scan_start(NULL, false);
    if(err != ESP_OK)
    {
        NW_LOGE(TAG, "esp_wifi_scan_start: 0x%x", err);
        return err;
    }
    if(!rtos_semTake(&_scanSem, RTOS_MS(timeoutMs)))
    {
        (void)esp_wifi_scan_stop();
        *pCount = 0;
        return ESP_ERR_TIMEOUT;
    }
    err = esp_wifi_scan_get_ap_num(&n);
    if(err != ESP_OK)
        return err;
    if(n > *pCount)
        n = *pCount;
    err = esp_wifi_scan_get_ap_records(&n, pRecords);
    *pCount = err == ESP_OK ? n : 0;
    (void)esp_wifi_clear_ap_list();
    return err;
}

esp_err_t nw_connectStart(const char *ssid, const char *password)
{
    wifi_config_t cfg;
    esp_err_t err;

    /* An attempt in progress (the policy's, or an earlier nw_connectStart's) must end
       before the library accepts a new configuration (ESP_ERR_WIFI_STATE otherwise). */
    nw_osi_timerStop(&_reconnTimer);
    if(_connecting)
    {
        _want = false;
        _connecting = false;
        (void)rtos_semTake(&_idleSem, RTOS_NO_WAIT);
        (void)esp_wifi_disconnect();
        if(!rtos_semTake(&_idleSem, RTOS_MS(3000)))
            NW_LOGW(TAG, "previous attempt did not end");
    }
    memset(&cfg, 0, sizeof(cfg));
    strncpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    if(password != NULL)
        strncpy((char *)cfg.sta.password, password, sizeof(cfg.sta.password));
    cfg.sta.threshold.authmode = password != NULL  &&  password[0] != 0 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    cfg.sta.pmf_cfg.capable = NW_CFG_PMF != 0;
    cfg.sta.pmf_cfg.required = false;
    err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if(err != ESP_OK)
    {
        NW_LOGE(TAG, "esp_wifi_set_config: 0x%x", err);
        return err;
    }
    nw_osi_timerStop(&_reconnTimer);
    _retriesLeft = NW_CFG_CONNECT_RETRIES;
    _backoffMs = NW_CFG_RECONNECT_MIN_MS;
    _connecting = true;
    _want = true;
    _link = NW_LINK_CONNECTING;
    _lastReason = 0;
    err = esp_wifi_connect();
    if(err != ESP_OK)
    {
        _connecting = false;
        _want = false;
        _link = NW_LINK_DOWN;
        NW_LOGE(TAG, "esp_wifi_connect: 0x%x", err);
        return err;
    }
    return ESP_OK;
}

esp_err_t nw_connect(const char *ssid, const char *password, uint32_t timeoutMs)
{
    esp_err_t err;

    (void)rtos_semTake(&_connSem, RTOS_NO_WAIT);
    _blocking = true;
    err = nw_connectStart(ssid, password);
    if(err != ESP_OK)
    {
        _blocking = false;
        return err;
    }
    if(!rtos_semTake(&_connSem, RTOS_MS(timeoutMs)))
    {
        _blocking = false;
        _want = false;
        _connecting = false;
        _link = NW_LINK_DOWN;
        (void)esp_wifi_disconnect();
        return ESP_ERR_TIMEOUT;
    }
    _blocking = false;
    if(!_connected)
    {
        _want = false;
        _link = NW_LINK_DOWN;
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t nw_disconnect(void)
{
    _want = false;
    nw_osi_timerStop(&_reconnTimer);
    _link = NW_LINK_DOWN;
    return esp_wifi_disconnect();
}

void nw_setReconnect(bool on)
{
    _auto = on;
    if(!on)
        nw_osi_timerStop(&_reconnTimer);
    else if(_want  &&  !_connected  &&  !_connecting)
        attempt();
}

uint8_t nw_linkState(void)
{
    return _link;
}

bool nw_isConnected(void)
{
    return _connected;
}

uint8_t nw_lastDisconnectReason(void)
{
    return _lastReason;
}

int8_t nw_rssi(void)
{
    wifi_ap_record_t rec;
    if(!_connected  ||  esp_wifi_sta_get_ap_info(&rec) != ESP_OK)
        return 0;
    return rec.rssi;
}

const uint8_t *nw_mac(void)
{
    return _mac;
}

void nw_getStats(nw_stats_t *pStats)
{
    rtos_heapStats_t hs;
    memset(pStats, 0, sizeof(*pStats));
    nw_osi_getStats(&pStats->events, &pStats->eventsDropped, &pStats->timerCallbacks, &pStats->isrs);
#if NW_CFG_NORNNET
    nw_netdev_getStats(&pStats->rxFrames, &pStats->rxDropped, &pStats->txFrames, &pStats->txErrors);
#endif
    rtos_heapStats(&hs);
    pStats->heapFree = (uint32_t)hs.free;
    pStats->heapMinFree = (uint32_t)hs.minFree;
    pStats->linkUps = _linkUps;
    pStats->linkDowns = _linkDowns;
    pStats->reconnectAttempts = _reconnectAttempts;
}
