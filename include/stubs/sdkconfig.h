#ifndef SDKCONFIG_INCLUDED
#define SDKCONFIG_INCLUDED
/* The ESP-IDF configuration the vendored headers see. It must describe the ESP32 the
   way the binary libraries were built, because some structures they share with us depend
   on it; the values are ESP-IDF v5.5.6 defaults for an ESP32 without PSRAM, NVS, WPA3,
   enterprise or coexistence. */
#define CONFIG_IDF_TARGET_ESP32                 1
#define CONFIG_IDF_TARGET                       "esp32"
#define CONFIG_SOC_WIFI_SUPPORTED               1
#define CONFIG_SOC_WIFI_ENABLED                 1
#define CONFIG_SOC_WIFI_NAN_SUPPORT             1
#define CONFIG_SOC_WIFI_WAPI_SUPPORT            1
#define CONFIG_SOC_WIFI_CSI_SUPPORT             1
#define CONFIG_SOC_WIFI_MESH_SUPPORT            1
#define CONFIG_ESP_WIFI_ENABLED                 1
#define CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM    10
#define CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM   32
#define CONFIG_ESP_WIFI_TX_BUFFER_TYPE          1
#define CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM   32
#define CONFIG_ESP_WIFI_DYNAMIC_RX_MGMT_BUF     0
#define CONFIG_ESP_WIFI_RX_MGMT_BUF_NUM_DEF     5
#define CONFIG_ESP_WIFI_AMPDU_TX_ENABLED        1
#define CONFIG_ESP_WIFI_TX_BA_WIN               6
#define CONFIG_ESP_WIFI_AMPDU_RX_ENABLED        1
#define CONFIG_ESP_WIFI_RX_BA_WIN               6
#define CONFIG_ESP_WIFI_ESPNOW_MAX_ENCRYPT_NUM  7
#define CONFIG_ESP_WIFI_MGMT_SBUF_NUM           32
#define CONFIG_ESP_WIFI_SOFTAP_BEACON_MAX_LEN   752
#define CONFIG_ESP_WIFI_SLP_DEFAULT_MIN_ACTIVE_TIME 50
#define CONFIG_ESP_WIFI_SLP_DEFAULT_MAX_ACTIVE_TIME 10
#define CONFIG_ESP_WIFI_SLP_DEFAULT_WAIT_BROADCAST_DATA_TIME 15
#define CONFIG_ESP_PHY_MAX_TX_POWER             20
#define CONFIG_LOG_MAXIMUM_LEVEL                3
#endif
