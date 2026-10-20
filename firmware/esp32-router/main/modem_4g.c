/**
 * @file modem_4g.c
 * @brief A7680C 4G LTE uplink driver for ESP32 NAT router.
 *
 * ═══════════════════════════════════════════════════════════════════════════
 * HARDWARE WIRING
 * ═══════════════════════════════════════════════════════════════════════════
 *
 *  ESP32                      A7680C module
 *  ─────────────────────────────────────────
 *  GPIO17 (UART TX)    ──►    RXD
 *  GPIO16 (UART RX)    ◄──    TXD
 *  GPIO5  (OUT)        ──►    NPN base [1kΩ] → Collector → PWRKEY
 *                             NPN Emitter → GND
 *  GPIO4  (IN)         ◄──    STATUS   (optional but strongly recommended)
 *  GND                 ───    GND
 *
 *  ⚠  POWER SUPPLY:
 *     A7680C VBAT = 3.4 V – 4.2 V, peak ~500 mA during TX.
 *     NEVER power from ESP32 3.3 V pin. Use a dedicated supply:
 *       • LDO: AMS1117-3.8, AP2114H-3.8, or similar
 *       • Li-ion cell (3.7 V nominal) ← simplest option
 *     Add 100 µF electrolytic + 100 nF ceramic on VBAT pin.
 *
 *  ⚠  PWRKEY LEVEL TRANSLATION:
 *     PWRKEY is pulled to VBAT (~3.8 V) internally via 50 kΩ.
 *     An ESP32 GPIO cannot drive it directly to GND safely.
 *     Use a small NPN transistor (2N2222, BC547, MMBT2222):
 *
 *       ESP32 GPIO5 ──[1kΩ]── Base
 *                             Collector ── PWRKEY pin
 *                             Emitter  ── GND
 *
 *     GPIO HIGH → transistor saturates → PWRKEY = GND (active)
 *     GPIO LOW  → transistor off       → PWRKEY floats to VBAT (idle)
 *
 * ═══════════════════════════════════════════════════════════════════════════
 * A7680C PWRKEY TIMING  (from A76XX Hardware Design document)
 * ═══════════════════════════════════════════════════════════════════════════
 *
 *  Power ON:
 *    1. VBAT stable
 *    2. PWRKEY idle HIGH
 *    3. Pull PWRKEY LOW  ≥ 1000 ms
 *    4. Release PWRKEY HIGH
 *    5. Wait for STATUS pin HIGH  (≈ 2–5 s after release)
 *    6. Module sends "RDY" on UART when ready for AT commands
 *       (only with fixed baud rate set via AT+IPR)
 *
 *  Power OFF:
 *    • Pull PWRKEY LOW  ≥ 1500 ms  (hardware path)
 *    • OR send AT+CPOF              (software path — preferred)
 *
 *  Reset (emergency only):
 *    • Pull RESET LOW  500–2000 ms
 *
 * ═══════════════════════════════════════════════════════════════════════════
 * DEPENDENCIES  (main/idf_component.yml)
 * ═══════════════════════════════════════════════════════════════════════════
 *
 *   espressif/esp_modem: ">=1.1.0"
 *
 * ═══════════════════════════════════════════════════════════════════════════
 * SDKCONFIG REQUIREMENTS
 * ═══════════════════════════════════════════════════════════════════════════
 *
 *   CONFIG_PPP_SUPPORT=y
 *   CONFIG_PPP_PAP_SUPPORT=y
 *   CONFIG_PPP_CHAP_SUPPORT=y
 *   CONFIG_LWIP_IP_FORWARD=y
 *   CONFIG_LWIP_IPV4_NAPT=y
 *   CONFIG_LWIP_L2_TO_L3_COPY=y
 */

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_system.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_ppp.h"
#include "esp_modem_api.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "modem_4g.h"

/* ─── Internal constants ──────────────────────────────────────────────────── */

static const char *TAG = "modem_4g";

#define NVS_NAMESPACE           "router_cfg"
#define NVS_KEY_APN             "apn"
#define NVS_KEY_APN_USER        "apn_user"
#define NVS_KEY_APN_PASS        "apn_pass"

/* A7680C timing */
#define PWRKEY_PULSE_ON_MS      1000   /* pull LOW this long to power ON      */
#define PWRKEY_PULSE_OFF_MS     1500   /* pull LOW this long to power OFF     */
#define BOOT_POLL_INTERVAL_MS    200   /* STATUS pin poll interval            */
#define BOOT_TIMEOUT_MS         8000   /* max wait for STATUS to go HIGH      */
#define AT_READY_GUARD_MS       2000   /* extra guard after STATUS HIGH       */

/* AT command retry policy */
#define SYNC_MAX_RETRIES          15
#define SYNC_RETRY_DELAY_MS      500

/* Network registration wait */
#define REG_TIMEOUT_S             60
#define PPP_IP_TIMEOUT_MS      90000   /* 90 s to obtain IP via PPP           */

/* Reconnect policy */
#define RECONNECT_DELAY_MS      8000

/* Event bits */
#define EVT_GOT_IP              BIT0
#define EVT_LOST_IP             BIT1

/* ─── Module state ────────────────────────────────────────────────────────── */

static esp_netif_t        *s_ppp_netif   = NULL;
static esp_modem_dce_t    *s_dce         = NULL;
static EventGroupHandle_t  s_evt_group   = NULL;
static volatile bool       s_connected   = false;

/* ─── Public accessors ────────────────────────────────────────────────────── */

esp_netif_t *modem_get_netif(void)    { return s_ppp_netif; }
bool         modem_is_connected(void) { return s_connected;  }

/* ══════════════════════════════════════════════════════════════════════════
 * NVS helpers
 * ══════════════════════════════════════════════════════════════════════════ */

esp_err_t modem_set_apn(const char *apn, const char *user, const char *pass)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    if (apn)  nvs_set_str(h, NVS_KEY_APN,      apn);
    if (user) nvs_set_str(h, NVS_KEY_APN_USER,  user);
    if (pass) nvs_set_str(h, NVS_KEY_APN_PASS,  pass);

    err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "APN saved: '%s'", apn ? apn : "(unchanged)");
    return err;
}

static void load_apn_config(char *apn,  size_t apn_sz,
                             char *user, size_t user_sz,
                             char *pass, size_t pass_sz)
{
    nvs_handle_t h;
    /* Start with hardcoded defaults */
    strlcpy(apn,  MODEM_DEFAULT_APN, apn_sz);
    user[0] = '\0';
    pass[0] = '\0';

    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_str(h, NVS_KEY_APN,      apn,  &apn_sz);
        nvs_get_str(h, NVS_KEY_APN_USER, user, &user_sz);
        nvs_get_str(h, NVS_KEY_APN_PASS, pass, &pass_sz);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "APN config — apn:'%s'  user:'%s'", apn, user);
}

/* ══════════════════════════════════════════════════════════════════════════
 * GPIO / PWRKEY helpers
 * ══════════════════════════════════════════════════════════════════════════ */

static void gpio_init_pwrkey(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << MODEM_PWRKEY_PIN),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    gpio_set_level(MODEM_PWRKEY_PIN, 0); /* transistor OFF = PWRKEY idle HIGH */
}

static void gpio_init_status(void)
{
    if (MODEM_STATUS_PIN < 0) return;
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << MODEM_STATUS_PIN),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
}

/** Returns true if STATUS pin is HIGH (module on), or false if pin is disabled */
static bool status_pin_high(void)
{
    if (MODEM_STATUS_PIN < 0) return false;
    return gpio_get_level((gpio_num_t)MODEM_STATUS_PIN) == 1;
}

/**
 * @brief  Block until STATUS goes HIGH or timeout_ms elapses.
 * @return true if STATUS went HIGH within timeout.
 */
static bool wait_for_status_high(uint32_t timeout_ms)
{
    if (MODEM_STATUS_PIN < 0) {
        /* No STATUS pin — just wait the full boot time blindly */
        vTaskDelay(pdMS_TO_TICKS(timeout_ms));
        return true; /* assume it booted */
    }
    uint32_t elapsed = 0;
    while (elapsed < timeout_ms) {
        if (status_pin_high()) return true;
        vTaskDelay(pdMS_TO_TICKS(BOOT_POLL_INTERVAL_MS));
        elapsed += BOOT_POLL_INTERVAL_MS;
    }
    return false;
}

/* ══════════════════════════════════════════════════════════════════════════
 * PWRKEY power-on / power-off sequences
 * ══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief  Full A7680C power-on sequence.
 *
 * Timeline (NPN transistor, GPIO HIGH = PWRKEY pulled LOW):
 *
 *  VBAT ────────────────────────────────────────────────────── stable
 *
 *  GPIO  _____|‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾|________________________
 *             ←── ≥ 1000 ms ──►
 *
 *  STATUS ____________________________|‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾
 *                                     ←── ≈ 2–5 s ──►
 *
 *  UART  (silent)                               "RDY\r\n" (if fixed baud)
 */
static void modem_power_on(void)
{
    gpio_init_pwrkey();
    gpio_init_status();

    /* If the module is already on, skip the pulse entirely.
     * This is important for soft-resets and watchdog reboots —
     * pulsing PWRKEY on an already-running module would power it OFF. */
    if (status_pin_high()) {
        ESP_LOGI(TAG, "PWRKEY: STATUS=HIGH — module already running, skipping pulse");
        vTaskDelay(pdMS_TO_TICKS(AT_READY_GUARD_MS));
        return;
    }

    ESP_LOGI(TAG, "PWRKEY: pulling LOW for %d ms (power-on pulse)...",
             PWRKEY_PULSE_ON_MS);

    /* Pull PWRKEY LOW */
    gpio_set_level(MODEM_PWRKEY_PIN, 1);          /* transistor ON */
    vTaskDelay(pdMS_TO_TICKS(PWRKEY_PULSE_ON_MS));

    /* Release PWRKEY */
    gpio_set_level(MODEM_PWRKEY_PIN, 0);          /* transistor OFF */
    ESP_LOGI(TAG, "PWRKEY: released — waiting for STATUS pin...");

    /* Wait for STATUS to go HIGH */
    bool booted = wait_for_status_high(BOOT_TIMEOUT_MS);
    if (booted) {
        ESP_LOGI(TAG, "PWRKEY: STATUS=HIGH, module is booting");
    } else {
        ESP_LOGW(TAG, "PWRKEY: STATUS still LOW after %d ms — check power supply / wiring!", BOOT_TIMEOUT_MS);
        /* Continue anyway — maybe STATUS is not wired */
    }

    /* Extra guard: AT commands need ≥ 2 s after power-on even when STATUS is up */
    ESP_LOGI(TAG, "PWRKEY: waiting %d ms for UART to be ready...", AT_READY_GUARD_MS);
    vTaskDelay(pdMS_TO_TICKS(AT_READY_GUARD_MS));
}

/**
 * @brief  Graceful power-off.
 *         Prefers AT+CPOF (software); falls back to PWRKEY hardware pulse.
 */
static void modem_power_off(void)
{
    if (s_dce != NULL) {
        ESP_LOGI(TAG, "Sending AT+CPOF for clean shutdown...");
        /* Must be in command mode to send AT */
        esp_modem_set_mode(s_dce, ESP_MODEM_MODE_COMMAND);
        vTaskDelay(pdMS_TO_TICKS(500));
        char resp[32] = {0};
        esp_modem_at(s_dce, "AT+CPOF", resp, 5000);
        vTaskDelay(pdMS_TO_TICKS(2000));
        return;
    }
    /* Hardware fallback */
    ESP_LOGI(TAG, "PWRKEY: hardware power-off pulse (%d ms)...", PWRKEY_PULSE_OFF_MS);
    gpio_set_level(MODEM_PWRKEY_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(PWRKEY_PULSE_OFF_MS));
    gpio_set_level(MODEM_PWRKEY_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(1000));
}

/* ══════════════════════════════════════════════════════════════════════════
 * Event handlers
 * ══════════════════════════════════════════════════════════════════════════ */

static void on_ip_event(void *arg, esp_event_base_t base,
                        int32_t id, void *data)
{
    if (id == IP_EVENT_PPP_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "┌─────────────────────────────────────┐");
        ESP_LOGI(TAG, "│  4G Connected!                      │");
        ESP_LOGI(TAG, "│  IP      : " IPSTR "               │", IP2STR(&evt->ip_info.ip));
        ESP_LOGI(TAG, "│  GW      : " IPSTR "               │", IP2STR(&evt->ip_info.gw));
        ESP_LOGI(TAG, "│  Mask    : " IPSTR "               │", IP2STR(&evt->ip_info.netmask));
        esp_netif_dns_info_t dns;
        esp_netif_get_dns_info(s_ppp_netif, ESP_NETIF_DNS_MAIN, &dns);
        ESP_LOGI(TAG, "│  DNS1    : " IPSTR "               │", IP2STR(&dns.ip.u_addr.ip4));
        esp_netif_get_dns_info(s_ppp_netif, ESP_NETIF_DNS_BACKUP, &dns);
        ESP_LOGI(TAG, "│  DNS2    : " IPSTR "               │", IP2STR(&dns.ip.u_addr.ip4));
        ESP_LOGI(TAG, "└─────────────────────────────────────┘");

        /* Make PPP the default route — all forwarded traffic exits via 4G.
         * Call it explicitly here (not just at init) so reconnects also
         * restore the default route correctly. */
        esp_netif_set_default_netif(s_ppp_netif);

        /* Verify — log which netif is actually default (useful for debugging) */
        esp_netif_t *def = esp_netif_get_default_netif();
        ESP_LOGI(TAG, "Default netif: %s", def ? esp_netif_get_desc(def) : "NULL");

        s_connected = true;
        xEventGroupSetBits(s_evt_group, EVT_GOT_IP);
        xEventGroupClearBits(s_evt_group, EVT_LOST_IP);

    } else if (id == IP_EVENT_PPP_LOST_IP) {
        ESP_LOGW(TAG, "4G: PPP lost IP");
        s_connected = false;
        xEventGroupClearBits(s_evt_group, EVT_GOT_IP);
        xEventGroupSetBits(s_evt_group, EVT_LOST_IP);
    }
}

static void on_ppp_status(void *arg, esp_event_base_t base,
                          int32_t id, void *data)
{
    if (id == NETIF_PPP_ERRORUSER) {
        ESP_LOGE(TAG, "PPP user error — likely wrong APN or auth credentials");
    }
    ESP_LOGD(TAG, "PPP status event id=%" PRId32, id);
}

/* ══════════════════════════════════════════════════════════════════════════
 * AT command helpers
 * ══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief  Probe modem with "AT" until it responds or retries exhausted.
 *         The module may still be booting when we first send commands.
 */
static esp_err_t modem_sync(void)
{
    char buf[16];
    for (int i = 0; i < SYNC_MAX_RETRIES; i++) {
        memset(buf, 0, sizeof(buf));
        esp_err_t err = esp_modem_at(s_dce, "AT", buf, 1000);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "AT sync OK (attempt %d/%d)", i + 1, SYNC_MAX_RETRIES);
            return ESP_OK;
        }
        ESP_LOGW(TAG, "AT sync attempt %d/%d: %s",
                 i + 1, SYNC_MAX_RETRIES, esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(SYNC_RETRY_DELAY_MS));
    }
    return ESP_ERR_TIMEOUT;
}

/**
 * @brief  Log modem identity and signal quality (informational only).
 */
static void modem_log_info(void)
{
    char buf[128] = {0};

    if (esp_modem_get_module_name(s_dce, buf) == ESP_OK)
        ESP_LOGI(TAG, "Module  : %s", buf);

    memset(buf, 0, sizeof(buf));
    if (esp_modem_get_imei(s_dce, buf) == ESP_OK)
        ESP_LOGI(TAG, "IMEI    : %s", buf);

    memset(buf, 0, sizeof(buf));
    if (esp_modem_get_operator_name(s_dce, buf, NULL) == ESP_OK)
        ESP_LOGI(TAG, "Operator: %s", buf);

    int rssi = 0, ber = 0;
    if (esp_modem_get_signal_quality(s_dce, &rssi, &ber) == ESP_OK) {
        const char *quality = rssi == 99 ? "unknown" :
                              rssi >= 20  ? "good"    :
                              rssi >= 10  ? "fair"    : "poor";
        ESP_LOGI(TAG, "Signal  : rssi=%d  ber=%d  (%s)", rssi, ber, quality);
    }
}

/**
 * @brief  Wait until the modem is registered on a network.
 *         Checks LTE (AT+CEREG) first, then falls back to AT+CREG (2G/3G).
 *         stat=1 → home network, stat=5 → roaming.
 */
static esp_err_t modem_wait_for_registration(void)
{
    ESP_LOGI(TAG, "Waiting for network registration (max %d s)...", REG_TIMEOUT_S);
    char resp[128];

    for (int s = 0; s < REG_TIMEOUT_S; s++) {
        memset(resp, 0, sizeof(resp));
        /* LTE registration */
        if (esp_modem_at(s_dce, "AT+CEREG?", resp, 2000) == ESP_OK) {
            if (strstr(resp, ",1") || strstr(resp, ",5")) {
                ESP_LOGI(TAG, "Registered (LTE/CEREG): %s", resp);
                return ESP_OK;
            }
        }
        /* GSM/WCDMA fallback */
        memset(resp, 0, sizeof(resp));
        if (esp_modem_at(s_dce, "AT+CREG?", resp, 2000) == ESP_OK) {
            if (strstr(resp, ",1") || strstr(resp, ",5")) {
                ESP_LOGI(TAG, "Registered (CREG): %s", resp);
                return ESP_OK;
            }
        }
        if (s % 5 == 0)
            ESP_LOGI(TAG, "  still searching... (%d s)", s);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGE(TAG, "Network registration timeout after %d s", REG_TIMEOUT_S);
    return ESP_ERR_TIMEOUT;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Reconnect watchdog task
 * ══════════════════════════════════════════════════════════════════════════ */

static void reconnect_task(void *arg)
{
    ESP_LOGI(TAG, "Reconnect watchdog started");

    while (1) {
        /* Wait indefinitely for a link-drop event */
        xEventGroupWaitBits(s_evt_group, EVT_LOST_IP,
                            pdTRUE,   /* clear on exit */
                            pdFALSE,
                            portMAX_DELAY);

        ESP_LOGW(TAG, "Link dropped — reconnecting in %d ms...", RECONNECT_DELAY_MS);
        vTaskDelay(pdMS_TO_TICKS(RECONNECT_DELAY_MS));

        /* Return to command mode */
        esp_err_t err = esp_modem_set_mode(s_dce, ESP_MODEM_MODE_COMMAND);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Cannot re-enter command mode (%s) — rebooting",
                     esp_err_to_name(err));
            esp_restart();
        }
        vTaskDelay(pdMS_TO_TICKS(1000));

        /* Check registration */
        if (modem_wait_for_registration() != ESP_OK) {
            ESP_LOGE(TAG, "Cannot re-register — rebooting");
            esp_restart();
        }

        /* Re-enter data mode */
        err = esp_modem_set_mode(s_dce, ESP_MODEM_MODE_DATA);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Cannot re-enter data mode (%s) — rebooting",
                     esp_err_to_name(err));
            esp_restart();
        }
        ESP_LOGI(TAG, "PPP data mode restarted — waiting for IP...");
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 * Public API
 * ══════════════════════════════════════════════════════════════════════════ */

void modem_4g_init(bool block)
{
    esp_err_t err;

    s_evt_group = xEventGroupCreate();
    configASSERT(s_evt_group);

    /* ── 1. Power on the A7680C ─────────────────────────────────────────── */
    modem_power_on();

    /* ── 2. Create the PPP netif (WAN side) ─────────────────────────────── */
    ESP_LOGI(TAG, "Creating PPP netif...");
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_PPP();
    s_ppp_netif = esp_netif_new(&netif_cfg);
    configASSERT(s_ppp_netif);

    /* ── 3. Register event handlers ──────────────────────────────────────── */
    ESP_ERROR_CHECK(esp_event_handler_register(
        IP_EVENT, ESP_EVENT_ANY_ID, &on_ip_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(
        NETIF_PPP_STATUS, ESP_EVENT_ANY_ID, &on_ppp_status, NULL));

    /* ── 4. Configure UART DTE ───────────────────────────────────────────── */
    esp_modem_dte_config_t dte_cfg = ESP_MODEM_DTE_DEFAULT_CONFIG();
    dte_cfg.uart_config.tx_io_num        = MODEM_TX_PIN;
    dte_cfg.uart_config.rx_io_num        = MODEM_RX_PIN;
    dte_cfg.uart_config.rts_io_num       = UART_PIN_NO_CHANGE;
    dte_cfg.uart_config.cts_io_num       = UART_PIN_NO_CHANGE;
    dte_cfg.uart_config.baud_rate        = 115200;
    dte_cfg.uart_config.rx_buffer_size   = 16384;   /* enlarge for PPP throughput */
    dte_cfg.uart_config.tx_buffer_size   = 2048;
    dte_cfg.uart_config.event_queue_size = 30;
    dte_cfg.task_stack_size              = 4096;
    dte_cfg.task_priority                = 10;
    dte_cfg.dte_buffer_size              = 4096;

    /* ── 5. Load APN from NVS ────────────────────────────────────────────── */
    char apn[64], user[32], pass[32];
    load_apn_config(apn, sizeof(apn), user, sizeof(user), pass, sizeof(pass));

    /* ── 6. Configure DCE ────────────────────────────────────────────────── */
    /*
     * The A7680C is SIMCom's ASR1602-based CAT1 module.
     * Its AT command set is SIM7600-compatible — use ESP_MODEM_DCE_SIM7600.
     */
    esp_modem_dce_config_t dce_cfg = ESP_MODEM_DCE_DEFAULT_CONFIG(apn);

    /* ── 7. Create DCE ───────────────────────────────────────────────────── */
    ESP_LOGI(TAG, "Creating DCE for A7680C (SIM7600-compatible)...");
    s_dce = esp_modem_new_dev(ESP_MODEM_DCE_SIM7600, &dte_cfg, &dce_cfg, s_ppp_netif);
    if (s_dce == NULL) {
        ESP_LOGE(TAG, "DCE creation failed — check UART wiring and power supply");
        return;
    }

    /* ── 8. AT sync ──────────────────────────────────────────────────────── */
    err = modem_sync();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Modem not responding to AT commands");
        ESP_LOGE(TAG, "  Check: TX/RX wiring, baud rate, power supply");
        return;
    }

    /* ── 9. Echo off ─────────────────────────────────────────────────────── */
    esp_modem_at(s_dce, "ATE0", NULL, 500);

    /* ── 10. Log identity and signal ─────────────────────────────────────── */
    modem_log_info();

    /* ── 11. Wait for network registration ───────────────────────────────── */
    err = modem_wait_for_registration();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Network registration failed — check SIM card and antenna");
        return;
    }

    /* ── 12. Start reconnect watchdog ────────────────────────────────────── */
    xTaskCreate(reconnect_task, "modem_reconnect", 3072, NULL, 4, NULL);

    /* ── 13. Enter PPP data mode ─────────────────────────────────────────── */
    ESP_LOGI(TAG, "Entering PPP data mode...");
    err = esp_modem_set_mode(s_dce, ESP_MODEM_MODE_DATA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enter PPP mode: %s", esp_err_to_name(err));
        return;
    }

    /* ── 14. Optionally block until IP is obtained ───────────────────────── */
    if (block) {
        ESP_LOGI(TAG, "Waiting for IP address (timeout %d ms)...", PPP_IP_TIMEOUT_MS);
        EventBits_t bits = xEventGroupWaitBits(
            s_evt_group, EVT_GOT_IP, pdFALSE, pdFALSE,
            pdMS_TO_TICKS(PPP_IP_TIMEOUT_MS));

        if (bits & EVT_GOT_IP) {
            ESP_LOGI(TAG, "4G uplink ready ✓");
        } else {
            ESP_LOGE(TAG, "Timed out waiting for PPP IP — check APN: '%s'", apn);
        }
    }
}

void modem_4g_deinit(void)
{
    if (s_dce) {
        modem_power_off();
        esp_modem_destroy(s_dce);
        s_dce = NULL;
    }
    if (s_ppp_netif) {
        esp_netif_destroy(s_ppp_netif);
        s_ppp_netif = NULL;
    }
    s_connected = false;
    ESP_LOGI(TAG, "4G uplink shut down");
}
