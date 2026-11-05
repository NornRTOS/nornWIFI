/* Not the ESP-IDF event loop: only the event base type the WiFi headers need. Events
   reach the application through nw_onEvent (nw.h). */
#ifndef NW_STUB_ESP_EVENT_INCLUDED
#define NW_STUB_ESP_EVENT_INCLUDED
#include "esp_event_base.h"
#endif
