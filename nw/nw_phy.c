/**
 * @file nw_phy.c
 * The chip side of the radio under nornOS: power domain, clock gates and resets of the
 * WiFi and PHY blocks, PHY initialisation and calibration through Espressif's libphy,
 * the MAC address from the efuse, the hardware random number generator. The register
 * addresses and sequences are those of ESP-IDF v5.5.6 for the ESP32 (periph_ctrl.c,
 * phy_init.c, esp_mac.c, esp_random.c).
 */

#include <string.h>
#include "rtos.h"
#include "port.h"
#include "nw_internal.h"
#include "esp_phy_init.h"
#include "phy_init_data.h"


/*
 * Defines
 */

#define REG(a)                  (*(volatile uint32_t *)(a))

#define DPORT_WIFI_CLK_EN       REG(0x3FF000CCul)
#define DPORT_CORE_RST_EN       REG(0x3FF000D0ul)
#define RTC_CNTL_DIG_PWC        REG(0x3FF48084ul)
#define RTC_CNTL_DIG_ISO        REG(0x3FF48088ul)
#define SENS_SAR_TSENS_CTRL     REG(0x3FF4884Cul)
#define EFUSE_BLK0_RDATA1       REG(0x3FF5A004ul)
#define EFUSE_BLK0_RDATA2       REG(0x3FF5A008ul)
#define WDEV_RND                REG(0x3FF75144ul)

#define WIFI_CLK_WIFI_BT_COMMON 0x000003C9ul    /* DPORT_WIFI_CLK_WIFI_BT_COMMON_M */
#define WIFI_CLK_WIFI_EN        0x00000406ul    /* DPORT_WIFI_CLK_WIFI_EN_M */
#define RTC_CNTL_WIFI_FORCE_PD  (1ul << 17)
#define RTC_CNTL_WIFI_FORCE_ISO (1ul << 28)
#define DPORT_WIFIMAC_RST       (1ul << 2)
#define MODEM_RESET_WHEN_PU     0x0000021Dul    /* WIFIBB, WIFIMAC, BTBB, BTMAC, RW_BTMAC */

#define TAG                     "phy"


/*
 * libphy entry points (esp_private/phy.h of ESP-IDF)
 */

extern void coex_bt_high_prio(void);     /* librtc: the Bluetooth side of the RF arbiter */
/* Hook for tests: called with a stage number during nw_phyEnable (1 after the
   calibration, 2 after phy_wifi_enable_set). */
void nw_phyStageHook(int stage) __attribute__((weak));
void nw_phyStageHook(int stage) { (void)stage; }
extern void phy_init_param_set(uint8_t param);   /* 1: WiFi + Bluetooth combo module (SOC_PHY_COMBO_MODULE) */
extern int register_chipv7_phy(const esp_phy_init_data_t *init_data, esp_phy_calibration_data_t *cal_data, esp_phy_calibration_mode_t cal_mode);
extern void phy_wakeup_init(void);
extern void phy_close_rf(void);
extern void phy_wifi_enable_set(uint8_t enable);
extern char *get_phy_version_str(void);
extern void ets_delay_us(uint32_t us);


/*
 * Local data
 */

static uint8_t _commonClockUsers = 0;
static bool _phyOn = false;
static bool _calibrated = false;
static uint32_t _rngState = 0x9E3779B9ul;


/*
 * Function implementation
 */

void nw_phyCommonClockEnable(void)
{
    const rtos_port_cs_t cs = rtos_port_enterCritical();
    if(_commonClockUsers++ == 0)
        DPORT_WIFI_CLK_EN |= WIFI_CLK_WIFI_BT_COMMON;
    rtos_port_exitCritical(cs);
}


void nw_phyCommonClockDisable(void)
{
    const rtos_port_cs_t cs = rtos_port_enterCritical();
    if(_commonClockUsers > 0  &&  --_commonClockUsers == 0)
        DPORT_WIFI_CLK_EN &= ~WIFI_CLK_WIFI_BT_COMMON;
    rtos_port_exitCritical(cs);
}


void nw_wifiClockEnable(void)
{
    const rtos_port_cs_t cs = rtos_port_enterCritical();
    DPORT_WIFI_CLK_EN |= WIFI_CLK_WIFI_EN;
    rtos_port_exitCritical(cs);
}


void nw_wifiClockDisable(void)
{
    const rtos_port_cs_t cs = rtos_port_enterCritical();
    DPORT_WIFI_CLK_EN &= ~WIFI_CLK_WIFI_EN;
    rtos_port_exitCritical(cs);
}


void nw_wifiResetMac(void)
{
    const rtos_port_cs_t cs = rtos_port_enterCritical();
    DPORT_CORE_RST_EN |= DPORT_WIFIMAC_RST;
    DPORT_CORE_RST_EN &= ~DPORT_WIFIMAC_RST;
    rtos_port_exitCritical(cs);
}


void nw_powerDomainOn(void)
{
    /* esp_wifi_bt_power_domain_on: power up, release the isolation, reset the modem. */
    RTC_CNTL_DIG_PWC &= ~RTC_CNTL_WIFI_FORCE_PD;
    ets_delay_us(10);
    nw_phyCommonClockEnable();
    DPORT_CORE_RST_EN |= MODEM_RESET_WHEN_PU;
    DPORT_CORE_RST_EN &= ~MODEM_RESET_WHEN_PU;
    RTC_CNTL_DIG_ISO &= ~RTC_CNTL_WIFI_FORCE_ISO;
    nw_phyCommonClockDisable();
}


/* How often the library switched the PHY (diagnostics; nw_osi_diagDump prints them). */
volatile uint32_t nw_phyEnables, nw_phyDisables, nw_phyWakeups;

void nw_phyEnable(void)
{
    ++ nw_phyEnables;
    if(!_phyOn)
    {
        nw_phyCommonClockEnable();
        /* The temperature sensor the PHY reads for its RF tracking is powered under
           ESP-IDF (SENS_TSENS_POWER_UP_FORCE | SENS_TSENS_POWER_UP, seen in a register
           comparison 2026-11-03); the ROM leaves it off. */
        SENS_SAR_TSENS_CTRL |= (1ul << 25) | (1ul << 24);
        /* ESP-IDF's phy_module_enable adds PHY_INIT_MODEM_CLOCK_REQUIRED_BITS for the
           calibration; on the ESP32 that value is not defined (0), the common clock is
           all it needs. (0x8F8F, the ESP32-C3/S3 value, used here until 2026-11-03,
           also clocked the Bluetooth baseband.) */
        if(!_calibrated)
        {
            esp_phy_calibration_data_t *pCal = nw_calloc(1, sizeof(*pCal));
            if(pCal == NULL)
                rtos_panic(RTOS_PANIC_APPLICATION, 0x4E57u, 1);
            NW_LOGI(TAG, "phy_version %s", get_phy_version_str());
            /* As esp_phy_load_cal_and_init: the ESP32 is a WiFi + Bluetooth combo
               module, which changes how the PHY sets up the shared RF front end. */
            phy_init_param_set(1);
            nw_readMac(pCal->mac, 0);
            {
                int r = register_chipv7_phy(&phy_init_data, pCal, PHY_RF_CAL_FULL);
                NW_LOGI(TAG, "full calibration returned %d, mac %02x:%02x:%02x:%02x:%02x:%02x", r,
                        pCal->mac[0], pCal->mac[1], pCal->mac[2], pCal->mac[3], pCal->mac[4], pCal->mac[5]);
#if NW_CFG_CAL_TWICE
                /* As a second boot under ESP-IDF: a partial calibration with the data the
                   full one produced (see nw_config.h). */
                phy_close_rf();
                r = register_chipv7_phy(&phy_init_data, pCal, PHY_RF_CAL_PARTIAL);
                NW_LOGI(TAG, "partial calibration returned %d", r);
#endif
            }
            nw_free(pCal);
            _calibrated = true;
        }
        else
        {
            ++ nw_phyWakeups;
            phy_wakeup_init();
        }
        coex_bt_high_prio();
        _phyOn = true;
        nw_phyStageHook(1);
    }
    phy_wifi_enable_set(1);
    nw_phyStageHook(2);
}


void nw_phyDisable(void)
{
    ++ nw_phyDisables;
    phy_wifi_enable_set(0);
    if(_phyOn)
    {
        phy_close_rf();
        _phyOn = false;
        nw_phyCommonClockDisable();
    }
}


int nw_readMac(uint8_t *mac, unsigned type)
{
    const uint32_t lo = EFUSE_BLK0_RDATA1, hi = EFUSE_BLK0_RDATA2;

    mac[0] = (uint8_t)(hi >> 8);
    mac[1] = (uint8_t)hi;
    mac[2] = (uint8_t)(lo >> 24);
    mac[3] = (uint8_t)(lo >> 16);
    mac[4] = (uint8_t)(lo >> 8);
    mac[5] = (uint8_t)lo;
    /* Four universally administered addresses per chip: station, soft AP, BT, Ethernet. */
    if(type <= 3)
        mac[5] = (uint8_t)(mac[5] + type);
    return 0;
}


uint32_t nw_random(void)
{
    /* The hardware generator is good only while the RF is on; it is mixed with a
       counter so that early calls differ too. */
    uint32_t x = _rngState ^ WDEV_RND ^ rtos_port_ccount();
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    _rngState = x;
    return x;
}


/* coex_bt_high_prio, which ESP-IDF's esp_phy_enable calls on the ESP32 after the
   calibration, is librtc's own (it sets up the Bluetooth side of the RF arbiter); until
   2026-11-03 an empty stub here replaced it. */


/* libphy's critical sections: the saved interrupt level travels through the value. */
uint32_t phy_enter_critical(void)
{
    return (uint32_t)rtos_port_enterCritical();
}

void phy_exit_critical(uint32_t level)
{
    rtos_port_exitCritical((rtos_port_cs_t)level);
}


/* The crystal frequency in MHz (rtc_clk_xtal_freq_get): the boards nornOS supports
   have 40 MHz, as the ROM detected when it printed "clock div:2". */
uint32_t rtc_get_xtal(void)
{
    return 40;
}
