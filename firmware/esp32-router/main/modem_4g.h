/**
 * @file modem_4g.h
 * @brief A7680C 4G LTE uplink driver — public API.
 *
 * Handles:
 *  - PWRKEY hardware power-on/off sequence
 *  - AT command sync and modem health check
 *  - Network registration wait
 *  - PPPoS data mode bringup
 *  - Automatic reconnect on link drop
 */

#pragma once

#include <stdbool.h>
#include "esp_netif.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Pin configuration ─────────────────────────────────────────────────────
 * Override these here or via menuconfig / compile flags.
 * See wiring notes in modem_4g.c.
 */
#ifndef MODEM_TX_PIN
#define MODEM_TX_PIN        17   /* ESP32 TX  →  A7680C RXD                  */
#endif
#ifndef MODEM_RX_PIN
#define MODEM_RX_PIN        16   /* ESP32 RX  ←  A7680C TXD                  */
#endif
#ifndef MODEM_PWRKEY_PIN
#define MODEM_PWRKEY_PIN     5   /* ESP32 OUT → NPN base → PWRKEY (see docs)  */
#endif
#ifndef MODEM_STATUS_PIN
#define MODEM_STATUS_PIN     4   /* ESP32 IN  ← A7680C STATUS (-1 to disable) */
#endif

/* ── APN defaults (override via NVS at runtime) ────────────────────────────
 * Common Vietnamese carriers:
 *   Viettel    : "v-internet"
 *   Mobifone   : "m-wap"
 *   Vinaphone  : "m3-world"
 *   Vietnamobile: "internet"
 */
#ifndef MODEM_DEFAULT_APN
#define MODEM_DEFAULT_APN   "v-internet"
#endif

/**
 * @brief Initialize the 4G uplink (power on modem, sync AT, register on
 *        network, bringup PPPoS). Registers the PPP netif as the default
 *        route so NAT traffic exits via 4G.
 *
 * Must be called after:
 *   - nvs_flash_init()
 *   - esp_netif_init()
 *   - esp_event_loop_create_default()
 *
 * @param block  true  → block until an IP address is obtained (recommended).
 *               false → return immediately; poll modem_is_connected().
 */
void modem_4g_init(bool block);

/**
 * @brief Gracefully shut down PPP and power off the modem.
 *        Sends AT+CPOF before the PWRKEY hardware pulse.
 */
void modem_4g_deinit(void);

/**
 * @brief  Returns the PPP netif handle (WAN side of the NAT router).
 * @return esp_netif_t* — valid after modem_4g_init(); NULL before.
 */
esp_netif_t *modem_get_netif(void);

/**
 * @brief  Returns true when PPPoS is up and an IP address has been assigned.
 */
bool modem_is_connected(void);

/**
 * @brief  Store APN credentials in NVS (persists across reboots).
 *         Pass NULL for user/pass if no authentication is required.
 */
esp_err_t modem_set_apn(const char *apn, const char *user, const char *pass);

#ifdef __cplusplus
}
#endif
