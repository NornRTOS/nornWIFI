/**
 * @file nw_supplicant.c
 * What the vendored supplicant (supplicant/, ESP-IDF's wpa_supplicant port) expects
 * from the parts nornWIF leaves out: WPS, WPA3, OWE, enterprise EAP and the soft AP.
 * The station side for WPA and WPA2 personal is compiled as it is, see nornwif.mk.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* WPS state machine callbacks (esp_wps.c): none registered. */
void *wps_get_wps_sm_cb(void)
{
    return NULL;
}

