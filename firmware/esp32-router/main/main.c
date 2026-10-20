/**
 * @file main.c
 * @brief ESP32 4G WiFi Router — application entry point.
 *
 * Boot sequence:
 *   1. NVS, event loop, netif stack init
 *   2. 4G uplink: power on A7680C, PPPoS bringup (blocks until IP)
 *   3. WiFi AP: start hotspot, enable NAPT
 *   4. Done — router is running
 *
 * Clients connect to the WiFi AP; their traffic is NATted through
 * the PPP interface to the 4G network.
 *
 * ─── Quick configuration ──────────────────────────────────────────────────
 *
 * APN (run once at first boot or whenever you change SIM):
 *   modem_set_apn("v-internet", NULL, NULL);   // Viettel, no auth
 *   modem_set_apn("m-wap", NULL, NULL);         // Mobifone
 *   modem_set_apn("m3-world", NULL, NULL);      // Vinaphone
 *
 * WiFi AP credentials:
 *   wifi_ap_set_config("MyRouter", "mypassword", 6);
 *
 * Changes are stored in NVS and persist across reboots.
 * Uncomment the one-shot calls below, flash, then comment them out again.
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs_flash.h"

#include "modem_4g.h"
#include "wifi_ap.h"
#include "ble_scanner.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    ESP_LOGI(TAG, "  ESP32 4G WiFi Router");
    ESP_LOGI(TAG, "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");

    /* ── System init ────────────────────────────────────────────────────── */

    /* NVS — required for WiFi driver and our config storage */
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition erased (version mismatch or full)");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    /* Global event loop — required by WiFi and netif */
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* LWIP network interface layer */
    ESP_ERROR_CHECK(esp_netif_init());

    modem_set_apn("v-internet", NULL, NULL);
    wifi_ap_set_config("ESP32_4G_Router", "CHANGE_ME_1234", 6);

    /* ── 4G uplink ───────────────────────────────────────────────────────
     * Powers on the A7680C, syncs AT, waits for registration,
     * brings up PPPoS, and blocks until an IP is obtained.
     * The PPP netif is set as the default route on success.
     */
    ESP_LOGI(TAG, "Starting 4G uplink...");
    modem_4g_init(true);   /* true = block until IP */

    if (!modem_is_connected()) {
        ESP_LOGE(TAG, "4G uplink failed — check modem wiring, SIM, and APN");
        ESP_LOGE(TAG, "Rebooting in 10 s...");
        vTaskDelay(pdMS_TO_TICKS(10000));
        esp_restart();
    }

    /* ── WiFi AP + NAPT ──────────────────────────────────────────────────
     * Starts the hotspot and enables IP masquerading so that AP clients
     * reach the internet via the 4G (PPP) uplink.
     * Must be called AFTER modem_4g_init() so the default route is set.
     */
    ESP_LOGI(TAG, "Starting WiFi AP...");
    esp_netif_t *ap_netif = wifi_ap_init();
    if (ap_netif == NULL) {
        ESP_LOGE(TAG, "WiFi AP failed to start — rebooting in 10 s...");
        vTaskDelay(pdMS_TO_TICKS(10000));
        esp_restart();
    }

    /* ── Push 4G DNS servers to AP DHCP clients ─────────────────────────
     * The PPP link received DNS servers from the carrier (e.g. 203.x.x.x).
     * Forward them into the AP's DHCP server so WiFi clients get working
     * DNS. Without this, clients get an IP but cannot resolve hostnames.
     */
    wifi_ap_push_dns(ap_netif, modem_get_netif());

    ESP_LOGI(TAG, "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");
    ESP_LOGI(TAG, "  Router is running!");
    ESP_LOGI(TAG, "  Connect to WiFi: " AP_DEFAULT_SSID);
    ESP_LOGI(TAG, "  Gateway / DNS  : 192.168.4.1");
    ESP_LOGI(TAG, "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━");

    /* ── Status loop ─────────────────────────────────────────────────────
     * Logs connectivity status every 60 s.
     * The reconnect logic lives inside modem_4g.c (reconnect_task).
     */
    ble_scanner_start();
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000));
        ESP_LOGI(TAG, "Heartbeat — 4G: %s",
                 modem_is_connected() ? "UP" : "DOWN (reconnecting...)");
    }
}
