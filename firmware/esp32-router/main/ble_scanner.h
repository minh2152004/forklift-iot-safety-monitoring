/**
 * ble_scanner.h
 *
 * iBeacon scanner for indoor zone tracking.
 * Scans BLE for beacons matching a known UUID, identifies the closest one
 * by RSSI, and publishes the resulting zone name to HiveMQ via MQTT/TLS.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Start BLE scanning and the zone publisher task.
 *         Must be called after WiFi / PPP / network are up.
 *         Initializes NimBLE, MQTT (TLS), and the periodic scan task.
 */
void ble_scanner_start(void);

#ifdef __cplusplus
}
#endif