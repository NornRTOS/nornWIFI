/* Not FreeRTOS: the vendored ESP-IDF headers include this for a queue handle type, and
   the supplicant's event loop for a delay. */
#ifndef NW_STUB_FREERTOS_INCLUDED
#define NW_STUB_FREERTOS_INCLUDED
#include <stdint.h>
#include "rtos.h"
typedef void *QueueHandle_t;
#define vTaskDelay(ticks)       rtos_delay((rtos_tick_t)(ticks))
#define portTICK_PERIOD_MS      (1000ul / RTOS_CFG_TICK_HZ)
#endif
