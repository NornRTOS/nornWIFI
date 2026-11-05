/**
 * @file nw_netdev.c
 * The station as a nornNET network device: Ethernet framed, the library copies frames
 * on transmit and hands received frames to a callback in its own task, where they are
 * copied into a pool buffer and queued for the net task. Built when nornwif.mk sees
 * NORNNET_DIR (NW_CFG_NORNNET).
 */

#include "nw_config.h"
#if NW_CFG_NORNNET

#include <string.h>
#include "nw_internal.h"
#include "esp_wifi.h"
#include "esp_private/wifi.h"
#include "nn_netdev.h"
#include "nn_net.h"
#include "nn_buf.h"
#include "nn_eth.h"
#include "nn_cfg.h"

#define TAG     "netdev"

static nn_netdev_t _dev;
static uint32_t _rxFrames, _rxDropped, _txFrames, _txErrors, _rxGroup;

static esp_err_t receive(void *buffer, uint16_t len, void *eb)
{
    nn_buf_t *pB = nn_bufAlloc(NN_CFG_RX_OFFSET);
    if(pB == NULL  ||  len > nn_bufTailroom(pB)  ||  len < NN_ETH_HDR_LEN)
    {
        if(pB != NULL)
            nn_bufFree(pB);
        ++ _rxDropped;
    }
    else
    {
        memcpy(nn_bufPut(pB, len), buffer, len);
        ++ _rxFrames;
        if(((const uint8_t *)buffer)[0] & 1)
            ++ _rxGroup;
        nn_net_rx(&_dev, pB);
    }
    esp_wifi_internal_free_rx_buffer(eb);
    return ESP_OK;
}

/* Optional hook for tests: called with the EtherType of every frame handed to the radio. */
void nw02_txHook(uint16_t etherType) __attribute__((weak));
void nw02_txHook(uint16_t etherType) { (void)etherType; }

static nn_err_t transmit(nn_netdev_t *pDev, nn_buf_t *pB)
{
    (void)pDev;
    if(!(_dev.flags & NN_DEVF_LINK))
        return NN_ERR_DOWN;
    if(esp_wifi_internal_tx(WIFI_IF_STA, pB->pData, pB->len) != ESP_OK)
    {
        ++ _txErrors;
        return NN_ERR_IO;
    }
    ++ _txFrames;
    if(pB->len >= 14)
        nw02_txHook((uint16_t)((pB->pData[12] << 8) | pB->pData[13]));
    nn_bufFree(pB);
    return NN_OK;
}

static const nn_netdevOps_t _ops = { NULL, NULL, transmit };

void nw_netdev_register(const uint8_t mac[6])
{
    memset(&_dev, 0, sizeof(_dev));
    _dev.name = NW_CFG_DEV_NAME;
    _dev.type = NN_DEV_ETHERNET;
    _dev.flags = NN_DEVF_BROADCAST;
    _dev.hwlen = NN_ETH_ALEN;
    memcpy(_dev.hwaddr, mac, NN_ETH_ALEN);
    _dev.mtu = NN_ETH_MTU;
    _dev.pOps = &_ops;
    if(nn_netdevRegister(&_dev) != NN_OK)
        NW_LOGE(TAG, "nn_netdevRegister failed");
}

/* From the service task, whose priority is above the stack lock's ceiling: nornNET
   itself may not be called here, so the change is posted to the net task, which drops
   the ARP entries on a loss and has DHCP confirm the lease on a return. */
void nw_netdev_link(bool up)
{
    if(up)
        esp_wifi_internal_reg_rxcb(WIFI_IF_STA, receive);
    else
        esp_wifi_internal_reg_rxcb(WIFI_IF_STA, NULL);
    nn_netdevLinkChanged(&_dev, up);
}

void nw_netdev_getStats(uint32_t *pRx, uint32_t *pRxDropped, uint32_t *pTx, uint32_t *pTxErrors)
{
    *pRx = _rxFrames;
    *pRxDropped = _rxDropped;
    *pTx = _txFrames;
    *pTxErrors = _txErrors;
}

uint32_t nw_netdev_rxGroup(void) { return _rxGroup; }

struct nn_netdev *nw_netdev(void)
{
    return &_dev;
}

#endif  /* NW_CFG_NORNNET */
