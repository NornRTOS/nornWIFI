/* ESP-IDF's log macros on top of the nornWIF log (nw_printf.c): the text carries its
   own prefix and newline, as the radio libraries' do. */
#ifndef NW_STUB_ESP_LOG_INCLUDED
#define NW_STUB_ESP_LOG_INCLUDED
#include <stdarg.h>
#include <stdint.h>
typedef enum { ESP_LOG_NONE, ESP_LOG_ERROR, ESP_LOG_WARN, ESP_LOG_INFO, ESP_LOG_DEBUG, ESP_LOG_VERBOSE } esp_log_level_t;
void esp_log_write(unsigned int level, const char *tag, const char *fmt, ...);
void esp_log_writev(unsigned int level, const char *tag, const char *fmt, va_list args);
uint32_t esp_log_timestamp(void);
#define ESP_LOG_LEVEL(level, tag, fmt, ...) esp_log_write(level, tag, "%c (%lu) %s: " fmt "\n", "NEWIDV"[level], (unsigned long)esp_log_timestamp(), tag, ##__VA_ARGS__)
#define ESP_LOG_LEVEL_LOCAL(level, tag, fmt, ...) ESP_LOG_LEVEL(level, tag, fmt, ##__VA_ARGS__)
#define ESP_LOGE(tag, fmt, ...) ESP_LOG_LEVEL(ESP_LOG_ERROR, tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) ESP_LOG_LEVEL(ESP_LOG_WARN, tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) ESP_LOG_LEVEL(ESP_LOG_INFO, tag, fmt, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) ESP_LOG_LEVEL(ESP_LOG_DEBUG, tag, fmt, ##__VA_ARGS__)
#define ESP_LOGV(tag, fmt, ...) ESP_LOG_LEVEL(ESP_LOG_VERBOSE, tag, fmt, ##__VA_ARGS__)
#define ESP_EARLY_LOGE ESP_LOGE
#define ESP_EARLY_LOGW ESP_LOGW
#define ESP_EARLY_LOGI ESP_LOGI
#define ESP_EARLY_LOGD ESP_LOGD
#define ESP_EARLY_LOGV ESP_LOGV
#define ESP_LOG_BUFFER_HEXDUMP(tag, buf, len, level) do { } while(0)
#define ESP_LOG_BUFFER_HEX_LEVEL(tag, buf, len, level) do { } while(0)
#endif
