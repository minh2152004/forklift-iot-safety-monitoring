/**
 * @file wifi_ap.h
 * @brief WiFi soft-AP (hotspot) driver — public API.
 *
 * Starts an 802.11 access point using the credentials stored in NVS.
 * The AP netif is registered with LWIP NAPT so clients' traffic
 * is NATted through the 4G (PPP) uplink.
 */

#pragma once

#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Default AP credentials (overridden at runtime via modem_set_ap_config) */
#ifndef AP_DEFAULT_SSID
#define AP_DEFAULT_SSID      "ESP32_4G_Router"
#endif
#ifndef AP_DEFAULT_PASS
#define AP_DEFAULT_PASS      "CHANGE_ME_1234"       /* min 8 chars for WPA2 */
#endif
#ifndef AP_DEFAULT_CHANNEL
#define AP_DEFAULT_CHANNEL   6
#endif
#ifndef AP_MAX_CLIENTS
#define AP_MAX_CLIENTS       5
#endif

/**
 * @brief  Initialize the WiFi soft-AP and enable NAPT on its netif.
 *
 * Must be called AFTER modem_4g_init() so the PPP netif is already
 * set as the default route. NAPT will forward packets from the AP
 * subnet out via the PPP default gateway.
 *
 * @return esp_netif_t* handle to the AP netif (or NULL on error).
 */
esp_netif_t *wifi_ap_init(void);

/**
 * @brief  Forward the DNS servers obtained from the PPP (4G) uplink into
 *         the AP's DHCP server so that WiFi clients receive working DNS.
 *
 * Call this once, immediately after wifi_ap_init() and modem_4g_init()
 * have both succeeded. The DNS addresses are read from ppp_netif and
 * written into ap_netif's DHCP server options.
 *
 * @param ap_netif   Handle returned by wifi_ap_init().
 * @param ppp_netif  Handle returned by modem_get_netif().
 */
void wifi_ap_push_dns(esp_netif_t *ap_netif, esp_netif_t *ppp_netif);

/**
 * @brief  Save AP credentials to NVS (takes effect after next reboot or
 *         call to wifi_ap_init()).
 */
void wifi_ap_set_config(const char *ssid, const char *password, uint8_t channel);

#ifdef __cplusplus
}
#endif
