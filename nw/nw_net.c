/**
 * @file nw_net.c
 * The common case with nornNET in three calls: nw_startNet() (the radio and one interface
 * on it), nw_dhcp() (an address for it) and nw_iface() (where it ended up). Everything here
 * is also reachable through nw_netdev() and nornNET's own calls; this file is for the
 * application that has one station and wants an address. Compiled only with NW_CFG_NORNNET
 * and linked only when called: two static objects in bss and a semaphore.
 */

#include "rtos.h"
#include "nw.h"
#include "nw_internal.h"

#if NW_CFG_NORNNET
#include "nn_net.h"
#include "nn_ip.h"
#include "nn_dhcp.h"

static nn_iface_t _iface;
static nn_dhcp_t _dhcp;
static rtos_sem_t _bound = RTOS_SEM_INIT(0);
static bool _started, _dhcpStarted;

static void onDhcp(nn_dhcp_t *pClient, bool bound, void *arg)
{
    (void)pClient; (void)arg;
    if(bound)
        rtos_semGive(&_bound);
}

esp_err_t nw_startNet(void)
{
    const esp_err_t err = nw_start();
    if(err != ESP_OK)
        return err;
    if(nn_ifaceAdd(nw_netdev(), &_iface, nn_ipAny(), nn_ipAny(), nn_ipAny()) != NN_OK)
        return ESP_FAIL;
    _started = true;
    return ESP_OK;
}

esp_err_t nw_dhcp(uint32_t waitMs)
{
    if(!_started)
        return ESP_FAIL;
    if(!_dhcpStarted)
    {
        if(nn_dhcpStart(&_dhcp, &_iface, onDhcp, NULL) != NN_OK)
            return ESP_FAIL;
        _dhcpStarted = true;
    }
    if(nn_dhcpIsBound(&_dhcp))
        return ESP_OK;
    if(waitMs == 0)
        return ESP_ERR_TIMEOUT;
    return rtos_semTake(&_bound, RTOS_MS(waitMs))  ||  nn_dhcpIsBound(&_dhcp) ? ESP_OK : ESP_ERR_TIMEOUT;
}

bool nw_dhcpBound(void)
{
    return _started  &&  nn_dhcpIsBound(&_dhcp);
}

esp_err_t nw_dhcpStop(void)
{
    if(!_dhcpStarted)
        return ESP_FAIL;
    _dhcpStarted = false;
    return nn_dhcpStop(&_dhcp) == NN_OK ? ESP_OK : ESP_FAIL;
}

const nn_iface_t *nw_iface(void)
{
    return _started ? &_iface : NULL;
}

#else
typedef int nw_net_unused_t;
#endif
