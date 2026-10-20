/**
 * @file wifi_ap.c
 * @brief WiFi soft-AP with LWIP NAPT for the ESP32 4G router.
 *
 * The AP hands out IPs in the 192.168.4.0/24 subnet via DHCP.
 * NAPT is enabled on the AP netif so that all AP client traffic
 * is masqueraded behind the PPP uplink IP.
 *
 * AP configuration (SSID / password / channel) is stored in NVS
 * and loaded at init time, falling back to compile-time defaults.
 */

#include <string.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "lwip/lwip_napt.h"

#include "wifi_ap.h"

static const char *TAG = "wifi_ap";

#define NVS_NAMESPACE    "router_cfg"
#define NVS_KEY_SSID     "ap_ssid"
#define NVS_KEY_PASS     "ap_pass"
#define NVS_KEY_CHAN     "ap_chan"

/* ─── NVS helpers ─────────────────────────────────────────────────────────── */

void wifi_ap_set_config(const char *ssid, const char *password, uint8_t channel)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    if (ssid)     nvs_set_str(h, NVS_KEY_SSID, ssid);
    if (password) nvs_set_str(h, NVS_KEY_PASS, password);
    nvs_set_u8(h, NVS_KEY_CHAN, channel);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "AP config saved: ssid='%s' ch=%d", ssid ? ssid : "?", channel);
}

static void load_ap_config(char *ssid, size_t ssid_sz,
                            char *pass, size_t pass_sz,
                            uint8_t *channel)
{
    strlcpy(ssid, AP_DEFAULT_SSID, ssid_sz);
    strlcpy(pass, AP_DEFAULT_PASS, pass_sz);
    *channel = AP_DEFAULT_CHANNEL;

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_str(h, NVS_KEY_SSID, ssid, &ssid_sz);
        nvs_get_str(h, NVS_KEY_PASS, pass, &pass_sz);
        nvs_get_u8(h,  NVS_KEY_CHAN, channel);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "AP config — ssid:'%s'  ch:%d", ssid, *channel);
}

/* ─── Event handler ───────────────────────────────────────────────────────── */

static void on_wifi_event(void *arg, esp_event_base_t base,
                          int32_t id, void *data)
{
    if (id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *evt = (wifi_event_ap_staconnected_t *)data;
        ESP_LOGI(TAG, "Client connected — MAC: %02x:%02x:%02x:%02x:%02x:%02x  AID:%d",
                 evt->mac[0], evt->mac[1], evt->mac[2],
                 evt->mac[3], evt->mac[4], evt->mac[5],
                 evt->aid);
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *evt = (wifi_event_ap_stadisconnected_t *)data;
        ESP_LOGI(TAG, "Client disconnected — MAC: %02x:%02x:%02x:%02x:%02x:%02x",
                 evt->mac[0], evt->mac[1], evt->mac[2],
                 evt->mac[3], evt->mac[4], evt->mac[5]);
    }
}

/* ─── Public API ──────────────────────────────────────────────────────────── */

esp_netif_t *wifi_ap_init(void)
{
    /* Load config */
    char ssid[33], pass[65];
    uint8_t channel;
    load_ap_config(ssid, sizeof(ssid), pass, sizeof(pass), &channel);

    /* Create the default AP netif.
     * esp_netif_create_default_wifi_ap() sets up the DHCP server
     * on 192.168.4.1 with a /24 pool automatically. */
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    if (ap_netif == NULL) {
        ESP_LOGE(TAG, "Failed to create AP netif");
        return NULL;
    }

    /* IDF v6.0: explicitly enable DNS option in DHCP offers.
     * Must be done before esp_wifi_start() triggers the DHCP server. */
    uint8_t dns_offer = 1;
    esp_netif_dhcps_stop(ap_netif);
    esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET,
                           ESP_NETIF_DOMAIN_NAME_SERVER,
                           &dns_offer, sizeof(dns_offer));
    esp_netif_dhcps_start(ap_netif);

    /* Init WiFi driver */
    wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_init_cfg));

    /* Register AP event handler */
    ESP_ERROR_CHECK(esp_event_handler_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, NULL));

    /* Configure AP */
    wifi_config_t ap_cfg = {
        .ap = {
            .channel         = channel,
            .max_connection  = AP_MAX_CLIENTS,
            .authmode        = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .required = false,
            },
        },
    };
    strlcpy((char *)ap_cfg.ap.ssid,     ssid, sizeof(ap_cfg.ap.ssid));
    strlcpy((char *)ap_cfg.ap.password, pass, sizeof(ap_cfg.ap.password));
    ap_cfg.ap.ssid_len = (uint8_t)strlen(ssid);

    /* Open AP if no password set */
    if (strlen(pass) == 0) {
        ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
        ESP_LOGW(TAG, "AP started with OPEN auth (no password)");
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "AP started — SSID:'%s'  CH:%d  Max clients:%d",
             ssid, channel, AP_MAX_CLIENTS);

    /* ── Enable NAPT on the AP netif ────────────────────────────────────────
     * NATtes all AP client traffic through the PPP default route.
     *
     * IMPORTANT: modem_4g_init(true) must have completed before this call
     * so that esp_netif_set_default_netif(ppp_netif) has already run.
     * The PPP netif being the default route is what makes NAPT send
     * forwarded packets out via 4G instead of looping back.
     *
     * Requires in sdkconfig:
     *   CONFIG_LWIP_IP_FORWARD=y
     *   CONFIG_LWIP_IPV4_NAPT=y
     *   CONFIG_LWIP_L2_TO_L3_COPY=y
     */
    esp_err_t err = esp_netif_napt_enable(ap_netif);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "NAPT enabled on AP netif ✓");
    } else {
        ESP_LOGE(TAG, "NAPT enable failed: %s  (check CONFIG_LWIP_IPV4_NAPT=y)",
                 esp_err_to_name(err));
    }

    return ap_netif;
}

void wifi_ap_push_dns(esp_netif_t *ap_netif, esp_netif_t *ppp_netif)
{
    if (!ap_netif || !ppp_netif) {
        ESP_LOGW(TAG, "wifi_ap_push_dns: NULL netif argument — skipping");
        return;
    }

    /* Read DNS servers from the PPP (4G) uplink */
    esp_netif_dns_info_t dns1 = {0};
    esp_netif_dns_info_t dns2 = {0};
    esp_netif_get_dns_info(ppp_netif, ESP_NETIF_DNS_MAIN,   &dns1);
    esp_netif_get_dns_info(ppp_netif, ESP_NETIF_DNS_BACKUP, &dns2);

    /*
     * IDF v6.0 breaking change: LWIP_DHCPS_ADD_DNS macro was removed.
     * To push DNS to DHCP clients you must now:
     *   1. Stop the DHCP server
     *   2. Enable DNS offer via esp_netif_dhcps_option()
     *   3. Set the DNS server via esp_netif_set_dns_info() (ESP_NETIF_DNS_MAIN only)
     *   4. Restart the DHCP server
     *
     * Note: lwIP DNS is global, not per-interface. Only ESP_NETIF_DNS_MAIN
     * is sent to clients via DHCP option 6. The backup server is used
     * locally by the ESP32 itself as a fallback resolver.
     */

    /* 1. Stop DHCP server so we can reconfigure it */
    esp_err_t err = esp_netif_dhcps_stop(ap_netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_LOGE(TAG, "Failed to stop DHCP server: %s", esp_err_to_name(err));
        return;
    }

    /* 2. Tell DHCP server to include DNS (option 6) in its offers */
    uint8_t dns_offer = 1;
    err = esp_netif_dhcps_option(ap_netif,
                                  ESP_NETIF_OP_SET,
                                  ESP_NETIF_DOMAIN_NAME_SERVER,
                                  &dns_offer,
                                  sizeof(dns_offer));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "dhcps_option DNS_OFFER failed: %s — DNS may still work via global lwIP",
                 esp_err_to_name(err));
    }

    /* 3. Set the DNS server that will be offered to clients.
     *    Only ESP_NETIF_DNS_MAIN is propagated via DHCP option 6. */
    err = esp_netif_set_dns_info(ap_netif, ESP_NETIF_DNS_MAIN, &dns1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_dns_info DNS_MAIN failed: %s", esp_err_to_name(err));
    }

    /* Also set globally so the ESP32's own resolver uses the carrier DNS */
    err = esp_netif_set_dns_info(ap_netif, ESP_NETIF_DNS_BACKUP, &dns2);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set_dns_info DNS_BACKUP failed: %s", esp_err_to_name(err));
    }

    /* 4. Restart DHCP server — new clients will now receive the DNS option */
    err = esp_netif_dhcps_start(ap_netif);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to restart DHCP server: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "DNS pushed to DHCP clients:");
    ESP_LOGI(TAG, "  DNS1 (offered): " IPSTR, IP2STR(&dns1.ip.u_addr.ip4));
    ESP_LOGI(TAG, "  DNS2 (backup) : " IPSTR, IP2STR(&dns2.ip.u_addr.ip4));
}
