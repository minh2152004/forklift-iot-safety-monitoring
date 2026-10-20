# ESP32 4G WiFi Router

Standalone ESP-IDF project that turns an **ESP32 + SIMCOM A7680C** into a
4G LTE WiFi router. The ESP32 brings up a PPPoS connection to the A7680C
over UART, then NATtes WiFi AP clients through that 4G uplink.

```
[Internet] ←→ [A7680C 4G LTE] ←UART/PPP→ [ESP32] ←WiFi AP→ [Clients]
```

---

## Hardware

### Wiring

| ESP32 Pin | A7680C Pin | Notes |
|-----------|-----------|-------|
| GPIO17 (TX) | RXD | UART transmit |
| GPIO16 (RX) | TXD | UART receive |
| GPIO5 (OUT) | PWRKEY (via NPN) | See transistor circuit below |
| GPIO4 (IN) | STATUS | Optional but recommended |
| GND | GND | Common ground |

### PWRKEY transistor circuit

PWRKEY is internally pulled to VBAT (~3.8 V). Use an NPN transistor:

```
ESP32 GPIO5 ──[1kΩ]── Base  (2N2222, BC547, MMBT2222)
                      Collector ── A7680C PWRKEY
                      Emitter  ── GND
```

GPIO HIGH → transistor ON → PWRKEY = GND (power on/off trigger)  
GPIO LOW  → transistor OFF → PWRKEY floats to VBAT (idle)

### Power supply

> ⚠️ **Do NOT power the A7680C from the ESP32 3.3 V pin.**

- VBAT range: **3.4 V – 4.2 V** (typical 3.8 V)
- Peak current: ~500 mA during LTE TX bursts
- Use a dedicated LDO (AMS1117-3.8, AP2114H-3.8) from 5 V, or a Li-ion cell
- Add **100 µF electrolytic + 100 nF ceramic** capacitors on VBAT

---

## Build & Flash

```bash
# 1. Clone / copy this project
cd esp32_4g_router

# 2. Set ESP-IDF target
idf.py set-target esp32

# 3. Configure (verify PPP + NAPT are enabled)
idf.py menuconfig

# 4. Build
idf.py build

# 5. Flash and monitor
idf.py -p /dev/ttyUSB0 flash monitor
```

The `idf_component.yml` will automatically fetch `espressif/esp_modem`
on the first build.

---

## Configuration

### APN (carrier)

Edit `main.c` and uncomment the one-shot call before the first flash:

```c
modem_set_apn("v-internet", NULL, NULL);   // Viettel
// modem_set_apn("m-wap",    NULL, NULL);  // Mobifone
// modem_set_apn("m3-world", NULL, NULL);  // Vinaphone
// modem_set_apn("internet", NULL, NULL);  // Vietnamobile
```

This writes to NVS and persists across reboots. Comment it out after
the first flash.

### WiFi AP credentials

```c
wifi_ap_set_config("MyRouter", "mypassword", 6);
```

Same pattern — uncomment, flash once, comment out.

### Pin overrides (without editing source)

Add to your `CMakeLists.txt` or pass via `idf.py -DMODEM_TX_PIN=17`:

```cmake
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    MODEM_TX_PIN=17
    MODEM_RX_PIN=16
    MODEM_PWRKEY_PIN=5
    MODEM_STATUS_PIN=4
    MODEM_DEFAULT_APN=\"v-internet\"
    AP_DEFAULT_SSID=\"ESP32_4G_Router\"
    AP_DEFAULT_PASS=\"CHANGE_ME_1234\"
)
```

---

## Project structure

```
esp32_4g_router/
├── CMakeLists.txt          # Root build file
├── sdkconfig.defaults      # Required Kconfig settings (PPP, NAPT, WiFi)
└── main/
    ├── CMakeLists.txt      # Component registration
    ├── idf_component.yml   # esp_modem managed component dependency
    ├── main.c              # app_main: init sequence + status loop
    ├── modem_4g.c          # A7680C driver: PWRKEY, PPPoS, reconnect
    ├── modem_4g.h
    ├── wifi_ap.c           # WiFi soft-AP + NAPT
    └── wifi_ap.h
```

---

## Troubleshooting

**Modem not responding to AT commands**
- Verify TX/RX are not swapped (ESP TX → modem RX)
- Check baud rate is 115200 on both ends
- The A7680C needs ~5 s after power-on before AT works — the driver
  handles this, but if you see timeouts, increase `AT_READY_GUARD_MS`

**STATUS pin always LOW**
- Check your power supply can deliver 500 mA peak
- Add bulk capacitance on VBAT (100 µF)
- If STATUS is not wired, set `MODEM_STATUS_PIN -1` — the driver
  will use timed waits instead

**PPP connects but no internet**
- Wrong APN for your carrier — check with your SIM provider
- Try `AT+CGDCONT?` in command mode to see the current PDP context

**NAPT not forwarding**
- Confirm `CONFIG_LWIP_IPV4_NAPT=y` and `CONFIG_LWIP_IP_FORWARD=y`
- `wifi_ap_init()` must be called AFTER `modem_4g_init()` so the
  default route is already set to the PPP interface
