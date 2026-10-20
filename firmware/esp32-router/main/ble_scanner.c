/**
 * ble_scanner.c
 *
 * NimBLE-based iBeacon scanner with HiveMQ Cloud (MQTT/TLS) publishing.
 *
 * Workflow:
 *   1. NimBLE host runs a passive scan window every SCAN_PERIOD_S seconds.
 *   2. Each advertisement is parsed; iBeacon frames matching BEACON_UUID
 *      and EXPECTED_MAJOR are recorded with their RSSI.
 *   3. After each window the strongest beacon's Minor field is mapped to
 *      a zone name from ZONE_MAP[].
 *   4. The current zone is published to HiveMQ on every scan cycle so
 *      the dashboard always sees a fresh timestamp.
 *
 * Coexistence with WiFi:
 *   - NimBLE host (lighter than Bluedroid)
 *   - Passive scan only (no probe requests)
 *   - Short scan windows (500ms / 10s) so WiFi keeps near-full throughput
 */

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_nimble_hci.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "mqtt_client.h"
#include "esp_crt_bundle.h"

#include "ble_scanner.h"
int ble_sm_alg_encrypt(const uint8_t *key, const uint8_t *plaintext, uint8_t *enc_data) {
    return 0;
}

static const char *TAG = "ble_scanner";

/* ════════════════════════════════════════════════════════════════════════
 *  CONFIGURATION
 * ════════════════════════════════════════════════════════════════════════ */

/* ── HiveMQ Cloud credentials ───────────────────────────────────────────── */
#define HIVEMQ_HOST       "YOUR-CLUSTER-ID.s1.eu.hivemq.cloud"
#define HIVEMQ_PORT       8883
#define HIVEMQ_USERNAME   "YOUR_MQTT_USER"
#define HIVEMQ_PASSWORD   "CHANGE_ME"

#define MQTT_TOPIC_ZONE   "router/location"

/* ── Beacon identification ──────────────────────────────────────────────── */
static const uint8_t BEACON_UUID[16] = {
    0xFD, 0xA5, 0x06, 0x93, 0xA4, 0xE2, 0x4F, 0xB1,
    0xAF, 0xCF, 0xC6, 0xEB, 0x07, 0x64, 0x78, 0x25
};

#define EXPECTED_MAJOR    0x0001

typedef struct {
    uint16_t    minor;
    const char *zone;
} zone_entry_t;

static const zone_entry_t ZONE_MAP[] = {
    { 0x0001, "Loading Bay"   },
    { 0x0002, "Storage Area"  },
    { 0x0003, "Assembly Line" },
    { 0x0004, "Office"        },
};
#define NUM_ZONES   (sizeof(ZONE_MAP) / sizeof(ZONE_MAP[0]))

/* ── Scan timing ──────────────────────────────────────────────────────── */
#define SCAN_WINDOW_MS     500    /* scan duration per cycle              */
#define SCAN_PERIOD_S      10     /* seconds between scan windows AND publishes */
#define RSSI_FLOOR        -95     /* discard signals weaker than this     */
#define ZONE_HYSTERESIS_DB  5     /* require N dBm advantage to switch    */

/* ════════════════════════════════════════════════════════════════════════
 *  STATE
 * ════════════════════════════════════════════════════════════════════════ */

static esp_mqtt_client_handle_t s_mqtt = NULL;
static bool                     s_mqtt_connected = false;

typedef struct {
    int8_t rssi;
    bool   seen;
} beacon_seen_t;

static beacon_seen_t s_beacons[NUM_ZONES];
static int           s_current_idx = -1;

/* ════════════════════════════════════════════════════════════════════════
 *  iBEACON PARSING
 * ════════════════════════════════════════════════════════════════════════ */

static bool parse_ibeacon(const uint8_t *data, uint8_t len,
                           uint16_t *out_major, uint16_t *out_minor)
{
    if (len < 25) return false;
    if (data[0] != 0x4C || data[1] != 0x00) return false;
    if (data[2] != 0x02 || data[3] != 0x15) return false;
    if (memcmp(&data[4], BEACON_UUID, 16) != 0) return false;

    *out_major = ((uint16_t)data[20] << 8) | data[21];
    *out_minor = ((uint16_t)data[22] << 8) | data[23];
    return true;
}

/* ════════════════════════════════════════════════════════════════════════
 *  BLE GAP CALLBACK
 * ════════════════════════════════════════════════════════════════════════ */

static int ble_scan_event(struct ble_gap_event *event, void *arg)
{
    if (event->type != BLE_GAP_EVENT_DISC) return 0;

    const struct ble_gap_disc_desc *d = &event->disc;
    if (d->rssi < RSSI_FLOOR) return 0;

    struct ble_hs_adv_fields fields;
    if (ble_hs_adv_parse_fields(&fields, d->data, d->length_data) != 0) {
        return 0;
    }
    if (fields.mfg_data_len < 25) return 0;

    uint16_t major, minor;
    if (!parse_ibeacon(fields.mfg_data, fields.mfg_data_len, &major, &minor)) {
        return 0;
    }
    if (major != EXPECTED_MAJOR) return 0;

    for (int i = 0; i < NUM_ZONES; i++) {
        if (ZONE_MAP[i].minor == minor) {
            if (!s_beacons[i].seen || d->rssi > s_beacons[i].rssi) {
                s_beacons[i].rssi = d->rssi;
                s_beacons[i].seen = true;
            }
            break;
        }
    }
    return 0;
}

/* ════════════════════════════════════════════════════════════════════════
 *  ZONE CLASSIFICATION + PUBLISH
 * ════════════════════════════════════════════════════════════════════════ */

static int classify_zone(int *out_rssi)
{
    int best_idx  = -1;
    int best_rssi = -127;

    for (int i = 0; i < NUM_ZONES; i++) {
        if (s_beacons[i].seen && s_beacons[i].rssi > best_rssi) {
            best_rssi = s_beacons[i].rssi;
            best_idx  = i;
        }
    }
    if (out_rssi) *out_rssi = best_rssi;

    if (s_current_idx >= 0 && best_idx != s_current_idx
        && s_beacons[s_current_idx].seen)
    {
        int curr_rssi = s_beacons[s_current_idx].rssi;
        if (best_rssi - curr_rssi < ZONE_HYSTERESIS_DB) {
            if (out_rssi) *out_rssi = curr_rssi;
            return s_current_idx;
        }
    }
    return best_idx;
}

static void publish_zone(int idx, int rssi)
{
    if (!s_mqtt || !s_mqtt_connected) return;

    char payload[160];
    int  n;

    if (idx >= 0) {
        n = snprintf(payload, sizeof(payload),
                     "{\"zone\":\"%s\",\"minor\":%u,\"rssi\":%d}",
                     ZONE_MAP[idx].zone,
                     (unsigned)ZONE_MAP[idx].minor,
                     rssi);
        ESP_LOGI(TAG, "→ HiveMQ: %s (rssi=%d)", ZONE_MAP[idx].zone, rssi);
    } else {
        n = snprintf(payload, sizeof(payload),
                     "{\"zone\":\"out of reach\",\"minor\":null,\"rssi\":null}");
        ESP_LOGI(TAG, "→ HiveMQ: out of reach");
    }

    esp_mqtt_client_publish(s_mqtt, MQTT_TOPIC_ZONE, payload, n, 1, 1);
}

/* ════════════════════════════════════════════════════════════════════════
 *  SCAN TASK
 * ════════════════════════════════════════════════════════════════════════ */

static void scan_task(void *arg)
{
    /* Wait for WiFi / PPP / MQTT to fully settle */
    vTaskDelay(pdMS_TO_TICKS(30000));

    for (;;) {
        memset(s_beacons, 0, sizeof(s_beacons));

        struct ble_gap_disc_params params = {
            .itvl              = 0x50,
            .window             = 0x30,
            .filter_policy     = BLE_HCI_SCAN_FILT_NO_WL,
            .limited           = 0,
            .passive           = 1,
            .filter_duplicates = 0,
        };

        int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, SCAN_WINDOW_MS,
                              &params, ble_scan_event, NULL);
        if (rc != 0 && rc != BLE_HS_EALREADY) {
            ESP_LOGW(TAG, "ble_gap_disc failed: rc=%d", rc);
        }

        vTaskDelay(pdMS_TO_TICKS(SCAN_WINDOW_MS + 100));
        ble_gap_disc_cancel();

        int rssi;
        int idx = classify_zone(&rssi);
        if (idx >= 0) {
            if (idx != s_current_idx) {
                ESP_LOGI(TAG, "Zone change: %s → %s",
                         s_current_idx >= 0 ? ZONE_MAP[s_current_idx].zone : "(none)",
                         ZONE_MAP[idx].zone);
                s_current_idx = idx;
            }
        } else {
            if (s_current_idx >= 0) {
                ESP_LOGI(TAG, "Zone change: %s → out of reach",
                         ZONE_MAP[s_current_idx].zone);
                s_current_idx = -1;
            }
        }
        /* Publish every cycle — either the current zone or "out of reach" */
        publish_zone(idx, rssi);

        vTaskDelay(pdMS_TO_TICKS((SCAN_PERIOD_S - 1) * 1000));
    }
}

/* ════════════════════════════════════════════════════════════════════════
 *  MQTT (HiveMQ Cloud — TLS + auth)
 * ════════════════════════════════════════════════════════════════════════ */

static void mqtt_event_handler(void *arg, esp_event_base_t base,
                                int32_t id, void *data)
{
    esp_mqtt_event_handle_t event = data;

    switch (id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "HiveMQ connected ✓");
            s_mqtt_connected = true;
            break;

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "HiveMQ disconnected — will retry automatically");
            s_mqtt_connected = false;
            break;

        case MQTT_EVENT_ERROR:
            if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
                ESP_LOGE(TAG, "TLS/transport error: 0x%x",
                         event->error_handle->esp_tls_last_esp_err);
            } else if (event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
                ESP_LOGE(TAG, "Connection refused — check username/password");
            }
            break;

        default:
            break;
    }
}

static void mqtt_init(void)
{
    esp_mqtt_client_config_t cfg = {
        .broker = {
            .address = {
                .hostname  = HIVEMQ_HOST,
                .port       = HIVEMQ_PORT,
                .transport = MQTT_TRANSPORT_OVER_SSL,
            },
            .verification = {
                .use_global_ca_store = false,
                .crt_bundle_attach   = esp_crt_bundle_attach,
            },
        },
        .credentials = {
            .username = HIVEMQ_USERNAME,
            .authentication = {
                .password = HIVEMQ_PASSWORD,
            },
            .client_id = "esp32-router-ble",
        },
        .session = {
            .keepalive               = 30,
            .disable_clean_session   = false,
        },
        .network = {
            .reconnect_timeout_ms = 5000,
            .timeout_ms           = 10000,
        },
    };

    s_mqtt = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(s_mqtt, ESP_EVENT_ANY_ID,
                                    mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_mqtt);
}

/* ════════════════════════════════════════════════════════════════════════
 *  NIMBLE HOST
 * ════════════════════════════════════════════════════════════════════════ */

static void ble_on_reset(int reason)
{
    ESP_LOGW(TAG, "BLE reset, reason=%d", reason);
}

static void ble_on_sync(void)
{
    ESP_LOGI(TAG, "BLE host synced — scanner ready");
}

static void ble_host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* ════════════════════════════════════════════════════════════════════════
 *  PUBLIC API
 * ════════════════════════════════════════════════════════════════════════ */

void ble_scanner_start(void)
{
    ESP_LOGI(TAG, "Starting BLE scanner — %d zones, %ds period",
             (int)NUM_ZONES, SCAN_PERIOD_S);

    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.reset_cb = ble_on_reset;
    ble_hs_cfg.sync_cb  = ble_on_sync;
    nimble_port_freertos_init(ble_host_task);

    mqtt_init();

    xTaskCreate(scan_task, "ble_scan", 4096, NULL, 3, NULL);
}