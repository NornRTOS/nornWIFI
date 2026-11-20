#ifndef NW_CMD_INCLUDED
#define NW_CMD_INCLUDED
/**
 * @file nw_cmd.h
 * @brief The `wifi` command of nornWIFI for the nosh shell (nornSHELL).
 *
 * Registered by the application with `nosh_add(&nw_cmd_wifi)`; linked in only then.
 * nornwif.mk compiles cmd/nw_cmd_wifi.c only when nosh.mk is among the application's
 * MODULES, so an application without the shell never sees this file.
 *
 *     wifi                      the link: state, rssi, MAC, last disconnect reason, counters
 *     wifi scan                 the networks in range, strongest first: ssid, rssi, channel, security
 *     wifi join <ssid> [psk]    leave the current network and join this one (non-blocking;
 *                               `wifi` shows the outcome); the psk is the rest of the line
 *     wifi join                 the same with the credentials the application supplies
 *                               (nw_cmd_setCredentials), e.g. the ones it keeps in a store
 *     wifi leave                disconnect and stop reconnecting
 *
 * The command knows nothing of where credentials live. An application that stores them
 * (nornStore, say) wires the two together with one callback; the module stays
 * independent of the store.
 */

#include "nosh.h"
#include "nw.h"

/** Fills ssid and psk (NUL terminated, the sizes are the buffers') with the credentials
    to use for `wifi join` without arguments; false if there are none. Runs in the shell
    task. */
typedef bool (*nw_cmd_credFunc_t)(char *ssid, size_t ssidSize, char *psk, size_t pskSize, void *arg);

/** Gives the command its source of credentials (NULL: none; `wifi join` then needs the
    ssid on the line). */
void nw_cmd_setCredentials(nw_cmd_credFunc_t fn, void *arg);

extern nosh_cmd_t nw_cmd_wifi;

#endif  /* NW_CMD_INCLUDED */
