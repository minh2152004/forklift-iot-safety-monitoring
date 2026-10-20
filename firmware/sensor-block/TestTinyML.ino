#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <HX711_ADC.h>
#include <Wire.h>
#include <math.h>

#include "model.h"
#include "certs.h"
#include "config.h"

#include "TensorFlowLite_ESP32.h"
#include "tensorflow/lite/micro/all_ops_resolver.h"
#include "tensorflow/lite/micro/micro_error_reporter.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/schema/schema_generated.h"

/* ---------------- WiFi ---------------- */

const char* ssid     = WIFI_SSID;
const char* password = WIFI_PASS;

/* ---------------- MQTT ---------------- */

const char* mqtt_server = MQTT_HOST;
const int   mqtt_port   = MQTT_PORT;
const char* mqtt_user   = MQTT_USER;
const char* mqtt_pass   = MQTT_PASS;

const char* mqtt_pub_data  = "forklift/data";
const char* mqtt_sub_alert = "forklift/alert";

WiFiClientSecure secureClient;
PubSubClient client(secureClient);

/* ---------------- HX711 ---------------- */

#define HX711_DOUT 4
#define HX711_SCK  5
#define BUZZER_PIN 15

HX711_ADC LoadCell(HX711_DOUT, HX711_SCK);

/* ---------------- MPU6050 ---------------- */

#define MPU_ADDR 0x68
#define MPU_SDA  19
#define MPU_SCL  18

// MPU6050 register map (only those we use)
#define MPU_REG_SMPLRT_DIV  0x19
#define MPU_REG_CONFIG      0x1A
#define MPU_REG_GYRO_CONFIG 0x1B
#define MPU_REG_ACCEL_CFG   0x1C
#define MPU_REG_ACCEL_XOUT  0x3B
#define MPU_REG_PWR_MGMT_1  0x6B

TwoWire I2C_MPU = TwoWire(0);

// NEW: bias offsets captured during startup calibration
float accel_off_x = 0, accel_off_y = 0, accel_off_z = 0;
float gyro_off_x  = 0, gyro_off_y  = 0, gyro_off_z  = 0;

// NEW: complementary-filter state (filtered angles, in degrees)
float roll_filt   = 0.0f;
float pitch_filt  = 0.0f;
unsigned long lastMPU_us = 0;

// NEW: filter coefficient — 0.96 trusts gyro short-term, accel long-term.
// Useful range 0.94 – 0.98. Higher = smoother but slower to track real tilt.
const float ALPHA = 0.96f;

/* ---------------- TinyML ---------------- */

constexpr int tensorArenaSize = 8 * 1024;
uint8_t tensorArena[tensorArenaSize];

tflite::MicroInterpreter* interpreter;
TfLiteTensor* input;
TfLiteTensor* output;

tflite::MicroErrorReporter errorReporter;

/* ---------------- Scaler ----------------
 * NOTE: these statistics were computed on raw (unfiltered) roll/pitch.
 * After enabling the complementary filter, the input distribution becomes
 * tighter (smaller std). Either retrain the MLP on filtered data and
 * regenerate these constants, or accept that the classifier was trained
 * on noisier data — it will still work, often better, since spurious
 * accel-induced spikes are now suppressed before reaching it.
 */
float weight_mean = 3.48535538;
float roll_mean   = -0.16247225;
float pitch_mean  = 0.27166038;

float weight_std = 2.74768411;
float roll_std   = 19.88144657;
float pitch_std  = 19.87395516;

/* ---------------- Timers ---------------- */

unsigned long lastSensor    = 0;
unsigned long lastMPURun    = 0;       // NEW: high-rate MPU loop timer
const unsigned long MPU_PERIOD_MS = 20;  // NEW: 50 Hz IMU sampling

bool buzzerActive = false;
unsigned long buzzerStartTime = 0;
const unsigned long buzzerDuration = 5000;

/* ---------------- WiFi Setup ---------------- */

void setup_wifi() {

  Serial.print("Connecting to WiFi");

  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
  }

  Serial.println();
  Serial.println("WiFi connected");
}

/* ---------------- MQTT ---------------- */

void reconnect() {

  while (!client.connected()) {

    Serial.print("Connecting MQTT...");

    String clientId = "ESP32-" + String(random(0xffff), HEX);

    if (client.connect(clientId.c_str(), mqtt_user, mqtt_pass)) {

      Serial.println("connected");
      client.subscribe(mqtt_sub_alert);

    } else {

      Serial.print("failed rc=");
      Serial.println(client.state());

      delay(3000);
    }
  }
}

void publishMessage(const char* topic, const char* payload) {

  client.publish(topic, payload);

}

/* ---------------- MPU helpers (NEW) ---------------- */

// Write one byte to one register on the MPU
void mpuWriteReg(uint8_t reg, uint8_t val) {

  I2C_MPU.beginTransmission(MPU_ADDR);
  I2C_MPU.write(reg);
  I2C_MPU.write(val);
  I2C_MPU.endTransmission(true);
}

// Configure DLPF + sensitivity ranges. Must be called after wake-up.
void configureMPU() {

  // Sample rate divider: with DLPF enabled, base = 1 kHz.
  // Divider = 4 → 200 Hz internal sampling, plenty for our 50 Hz read loop.
  mpuWriteReg(MPU_REG_SMPLRT_DIV, 0x04);

  // DLPF_CFG = 3 → accel BW ~44 Hz, gyro BW ~42 Hz.
  // This kills engine/road vibration above 50 Hz.
  mpuWriteReg(MPU_REG_CONFIG, 0x03);

  // Gyro full-scale = ±250 °/s → 131 LSB/°/s
  mpuWriteReg(MPU_REG_GYRO_CONFIG, 0x00);

  // Accel full-scale = ±2 g → 16384 LSB/g  (matches existing code)
  mpuWriteReg(MPU_REG_ACCEL_CFG, 0x00);
}

// Capture accelerometer + gyroscope biases.
// The forklift MUST be level and motionless for the duration of this call.
void calibrateMPU() {

  const int N = 500;
  long ax = 0, ay = 0, az = 0;
  long gx = 0, gy = 0, gz = 0;

  Serial.println("Calibrating MPU — keep forklift level and still…");

  for (int i = 0; i < N; i++) {

    I2C_MPU.beginTransmission(MPU_ADDR);
    I2C_MPU.write(MPU_REG_ACCEL_XOUT);
    I2C_MPU.endTransmission(false);
    I2C_MPU.requestFrom(MPU_ADDR, 14, true);

    ax += (int16_t)(I2C_MPU.read() << 8 | I2C_MPU.read());
    ay += (int16_t)(I2C_MPU.read() << 8 | I2C_MPU.read());
    az += (int16_t)(I2C_MPU.read() << 8 | I2C_MPU.read());
    I2C_MPU.read(); I2C_MPU.read();        // temperature (discarded)
    gx += (int16_t)(I2C_MPU.read() << 8 | I2C_MPU.read());
    gy += (int16_t)(I2C_MPU.read() << 8 | I2C_MPU.read());
    gz += (int16_t)(I2C_MPU.read() << 8 | I2C_MPU.read());

    delay(3);
  }

  accel_off_x = (float)ax / N;
  accel_off_y = (float)ay / N;
  accel_off_z = (float)az / N - 16384.0f;   // subtract +1 g baseline on Z

  gyro_off_x  = (float)gx / N;
  gyro_off_y  = (float)gy / N;
  gyro_off_z  = (float)gz / N;

  Serial.printf("  accel offset = (%.0f, %.0f, %.0f) LSB\n",
                accel_off_x, accel_off_y, accel_off_z);
  Serial.printf("  gyro  offset = (%.0f, %.0f, %.0f) LSB\n",
                gyro_off_x, gyro_off_y, gyro_off_z);

  // Seed the complementary filter with the (now bias-corrected)
  // accelerometer reading so it doesn't have to slew from zero.
  I2C_MPU.beginTransmission(MPU_ADDR);
  I2C_MPU.write(MPU_REG_ACCEL_XOUT);
  I2C_MPU.endTransmission(false);
  I2C_MPU.requestFrom(MPU_ADDR, 6, true);
  int16_t rax = I2C_MPU.read() << 8 | I2C_MPU.read();
  int16_t ray = I2C_MPU.read() << 8 | I2C_MPU.read();
  int16_t raz = I2C_MPU.read() << 8 | I2C_MPU.read();

  float AccX = (rax - accel_off_x) / 16384.0f;
  float AccY = (ray - accel_off_y) / 16384.0f;
  float AccZ = (raz - accel_off_z) / 16384.0f;

  roll_filt  = atan2f(AccY, AccZ) * 180.0f / PI;
  pitch_filt = atanf(-AccX / sqrtf(AccY * AccY + AccZ * AccZ)) * 180.0f / PI;
}

// Read accel + gyro, update roll_filt / pitch_filt via complementary filter.
// Called at ~50 Hz from loop().
void readMPU() {

  I2C_MPU.beginTransmission(MPU_ADDR);
  I2C_MPU.write(MPU_REG_ACCEL_XOUT);
  I2C_MPU.endTransmission(false);
  I2C_MPU.requestFrom(MPU_ADDR, 14, true);

  int16_t rax = I2C_MPU.read() << 8 | I2C_MPU.read();
  int16_t ray = I2C_MPU.read() << 8 | I2C_MPU.read();
  int16_t raz = I2C_MPU.read() << 8 | I2C_MPU.read();
  I2C_MPU.read(); I2C_MPU.read();        // temperature, ignored
  int16_t rgx = I2C_MPU.read() << 8 | I2C_MPU.read();
  int16_t rgy = I2C_MPU.read() << 8 | I2C_MPU.read();
  I2C_MPU.read(); I2C_MPU.read();        // gyro_z, not used for tilt

  // Bias-corrected and scaled values
  float AccX  = (rax - accel_off_x) / 16384.0f;          // g
  float AccY  = (ray - accel_off_y) / 16384.0f;
  float AccZ  = (raz - accel_off_z) / 16384.0f;
  float GyroX = (rgx - gyro_off_x)  / 131.0f;            // °/s
  float GyroY = (rgy - gyro_off_y)  / 131.0f;

  // Instantaneous accelerometer-derived angles (noisy, drift-free)
  float roll_acc  = atan2f(AccY, AccZ) * 180.0f / PI;
  float pitch_acc = atanf(-AccX / sqrtf(AccY * AccY + AccZ * AccZ))
                    * 180.0f / PI;

  // Time delta in seconds
  unsigned long now = micros();
  float dt = (lastMPU_us == 0) ? 0.02f : (now - lastMPU_us) * 1e-6f;
  lastMPU_us = now;
  if (dt > 0.1f) dt = 0.02f;             // sanity guard after long pauses

  // Complementary filter:
  //   trust the gyro for high-frequency motion (immune to linear accel),
  //   use the accelerometer to slowly correct gyro drift.
  roll_filt  = ALPHA * (roll_filt  + GyroX * dt) + (1.0f - ALPHA) * roll_acc;
  pitch_filt = ALPHA * (pitch_filt + GyroY * dt) + (1.0f - ALPHA) * pitch_acc;
}

/* ---------------- MQTT Callback ---------------- */

void callback(char* topic, byte* payload, unsigned int length) {

  String message = "";

  for (int i = 0; i < length; i++) {
    message += (char)payload[i];
  }

  Serial.print("Message arrived [");
  Serial.print(topic);
  Serial.print("]: ");
  Serial.println(message);

  if (String(topic) == mqtt_sub_alert) {

    if (message == "DANGER") {

      Serial.println("DANGER detected -> Buzzer ON (5s)");

      buzzerActive = true;
      buzzerStartTime = millis();
      digitalWrite(BUZZER_PIN, HIGH);
    }
  }
}

/* ---------------- Setup ---------------- */

void setup() {

  Serial.begin(115200);

  setup_wifi();

  secureClient.setCACert(ca_cert);
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);

  /* HX711 */

  LoadCell.begin();
  LoadCell.start(2000);
  LoadCell.setCalFactor(22.47);
  LoadCell.setSamplesInUse(4);

  Serial.println("Loadcell ready");

  /* MPU6050 — wake up, configure, calibrate */

  I2C_MPU.begin(MPU_SDA, MPU_SCL);
  I2C_MPU.setClock(400000);              // NEW: 400 kHz I²C for faster reads

  // Wake the device from sleep
  mpuWriteReg(MPU_REG_PWR_MGMT_1, 0x00);
  delay(100);

  // NEW: set DLPF + sensitivity ranges before calibration
  configureMPU();

  // NEW: capture bias offsets — vehicle must be level and still
  calibrateMPU();

  Serial.println("MPU6050 ready");

  /* Buzzer */

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  /* TinyML */

  const tflite::Model* model = tflite::GetModel(model_tflite);
  static tflite::AllOpsResolver resolver;
  static tflite::MicroInterpreter static_interpreter(
      model,
      resolver,
      tensorArena,
      tensorArenaSize,
      &errorReporter);

  interpreter = &static_interpreter;
  interpreter->AllocateTensors();

  input  = interpreter->input(0);
  output = interpreter->output(0);

  Serial.println("TinyML ready");
}

/* ---------------- LOOP ---------------- */

void loop() {

  if (!client.connected()) reconnect();
  client.loop();

  LoadCell.update();

  /* Buzzer timeout */

  if (buzzerActive && millis() - buzzerStartTime >= buzzerDuration) {
    digitalWrite(BUZZER_PIN, LOW);
    buzzerActive = false;
    Serial.println("Buzzer OFF");
  }

  /* NEW: high-rate MPU update (~50 Hz) */

  if (millis() - lastMPURun >= MPU_PERIOD_MS) {
    lastMPURun = millis();
    readMPU();
  }

  /* SENSOR + TINYML PUBLISH (1 s) */

  if (millis() - lastSensor > 1000) {

    lastSensor = millis();

    // --- Read load + take a snapshot of the filtered angles ---
    float weightKg = LoadCell.getData() / 1000.0f;
    if (weightKg < 0.01f) weightKg = 0;

    float roll  = roll_filt;
    float pitch = pitch_filt;
    float tilt  = sqrtf(roll * roll + pitch * pitch); 

    // --- TinyML inference ---
    float weight_scaled = (weightKg - weight_mean) / weight_std;
    float roll_scaled   = (roll - roll_mean) / roll_std;
    float pitch_scaled  = (pitch - pitch_mean) / pitch_std;

    input->data.f[0] = weight_scaled;
    input->data.f[1] = roll_scaled;
    input->data.f[2] = pitch_scaled;

    interpreter->Invoke();

    int predicted = 0;
    float maxVal = output->data.f[0];
    for (int i = 1; i < 4; i++) {
      if (output->data.f[i] > maxVal) {
        maxVal = output->data.f[i];
        predicted = i;
      }
    }

    const char* labels[] = {"idle", "normal", "overload", "unbalance"};
    const char* status = labels[predicted];

    Serial.printf("W %.2f kg | R %+6.2f | P %+6.2f | T %5.2f | %s\n",
                  weightKg, roll, pitch, tilt, status);

    if (predicted == 2 || predicted == 3) {
      buzzerActive = true;
      buzzerStartTime = millis();
      digitalWrite(BUZZER_PIN, HIGH);
    }

    // --- Build single combined payload ---
    String payload = "{";
    payload += "\"weight_kg\":" + String(weightKg, 2) + ",";
    payload += "\"tilt\":"      + String(tilt, 2)    + ",";     
    payload += "\"status\":\""  + String(status)     + "\"";
    payload += "}";

    publishMessage(mqtt_pub_data, payload.c_str());
  }
}