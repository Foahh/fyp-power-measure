#include <Adafruit_INA228.h>
#include <Wire.h>

#ifndef MANUAL_MODE
#define MANUAL_MODE 0
#endif

#if !MANUAL_MODE
#include "pb_encode.h"
#include "power_sample.pb.h"
#endif

Adafruit_INA228 ina228;

static constexpr uint8_t INA228_I2C_ADDR = INA228_I2CADDR_DEFAULT;
static constexpr float SHUNT_RESISTOR_OHMS = 0.1f;
static constexpr float MAX_EXPECTED_CURRENT_A = 1.0f;

static constexpr int INA228_SDA_PIN = 2;
static constexpr int INA228_SCL_PIN = 1;

static constexpr int IS_INFERENCING_PIN = 3;
static constexpr char PM_PING_CMD[] = "PM_PING";

#if MANUAL_MODE
static constexpr uint32_t CONSTANT_SAMPLE_PERIOD_US = 0;
#endif

#if !MANUAL_MODE
static uint8_t pb_buf[PowerSample_size];

static volatile bool edge_detected = false;
static volatile bool edge_pin_high = false;

static void IRAM_ATTR onSyncEdge() {
  edge_pin_high = digitalRead(IS_INFERENCING_PIN) == HIGH;
  edge_detected = true;
}

static uint32_t window_start_us = 0;
#endif

static char serial_cmd_buf[32];
static size_t serial_cmd_len = 0;

static void sendAckLine() {
  const int sync = digitalRead(IS_INFERENCING_PIN) == HIGH;
  Serial.printf("PM_ACK ina228=1 sync=%d ts_us=%lu\n", sync,
                (unsigned long)micros());
}

static void processSerialCommand(const char *cmd) {
  if (strcmp(cmd, PM_PING_CMD) == 0) {
    sendAckLine();
  }
}

static void serviceSerialCommands() {
  while (Serial.available() > 0) {
    const int ch = Serial.read();
    if (ch < 0) return;

    if (ch == '\n' || ch == '\r') {
      if (serial_cmd_len > 0) {
        serial_cmd_buf[serial_cmd_len] = '\0';
        processSerialCommand(serial_cmd_buf);
        serial_cmd_len = 0;
      }
      continue;
    }

    if (serial_cmd_len < sizeof(serial_cmd_buf) - 1) {
      serial_cmd_buf[serial_cmd_len++] = (char)ch;
    } else {
      serial_cmd_len = 0;
    }
  }
}

#if !MANUAL_MODE
static void sendSample(uint32_t ts, float energy_j, uint32_t duration_us,
                       bool is_inference) {
  PowerSample sample = PowerSample_init_zero;
  sample.timestamp_us = ts;
  sample.energy_j = max(energy_j, 0.0f);
  sample.duration_us = duration_us;
  sample.is_inference = is_inference;

  pb_ostream_t stream = pb_ostream_from_buffer(pb_buf, sizeof(pb_buf));
  if (!pb_encode(&stream, PowerSample_fields, &sample)) return;

  const uint32_t len = (uint32_t)stream.bytes_written;
  const uint8_t prefix[4] = {
      (uint8_t)(len),
      (uint8_t)(len >> 8),
      (uint8_t)(len >> 16),
      (uint8_t)(len >> 24),
  };
  Serial.write(prefix, 4);
  Serial.write(pb_buf, len);
}
#endif

void setup() {
  Serial.begin(921600);
  delay(200);

  Wire.begin(INA228_SDA_PIN, INA228_SCL_PIN);
  Wire.setClock(400000);
  pinMode(IS_INFERENCING_PIN, INPUT_PULLDOWN);

#if MANUAL_MODE
  Serial.println("# PM MANUAL_MODE: "
                 "ts_us,vbus_v,current_a,power_w,energy_j,sync");
#endif

  while (!ina228.begin(INA228_I2C_ADDR, &Wire)) {
    Serial.println("# ERROR: INA228 not found.");
    delay(1000);
  }

  ina228.setShunt(SHUNT_RESISTOR_OHMS, MAX_EXPECTED_CURRENT_A);

  // No averaging — energy accumulator already integrates across conversion
  // cycles. Temperature channel skipped to halve cycle time.
  ina228.setAveragingCount(INA228_COUNT_1);
  ina228.setVoltageConversionTime(INA228_TIME_50_us);
  ina228.setCurrentConversionTime(INA228_TIME_50_us);
  ina228.setMode(INA228_MODE_CONT_BUS_SHUNT);
  ina228.resetAccumulators();

#if !MANUAL_MODE
  window_start_us = micros();
  attachInterrupt(digitalPinToInterrupt(IS_INFERENCING_PIN), onSyncEdge,
                  CHANGE);
#endif
}

void loop() {
  serviceSerialCommands();

#if MANUAL_MODE
  static uint32_t next_sample_us = 0;
  const uint32_t now = micros();
  if (CONSTANT_SAMPLE_PERIOD_US != 0 && (int32_t)(now - next_sample_us) < 0)
    return;
  next_sample_us = now + CONSTANT_SAMPLE_PERIOD_US;

  const float vbus_v = ina228.readBusVoltage();
  const float current_a = ina228.readCurrent();
  const float power_w = ina228.readPower();
  const float energy_j = max(ina228.readEnergy(), 0.0f);
  const int sync = digitalRead(IS_INFERENCING_PIN) == HIGH;
  Serial.printf("%lu,%.6f,%.6f,%.6f,%.9f,%d\n", (unsigned long)now, vbus_v,
                current_a, power_w, energy_j, sync);
#else
  if (!edge_detected) return;
  edge_detected = false;

  const uint32_t ts = micros();
  const bool sync_now = edge_pin_high;

  float energy_j = ina228.readEnergy();
  const uint32_t duration_us = ts - window_start_us;
  if (duration_us == 0 || energy_j < 0.0f) energy_j = 0.0f;

  ina228.resetAccumulators();
  window_start_us = ts;

  // Rising edge (sync_now=true) → idle window ended.
  // Falling edge (sync_now=false) → inference window ended.
  sendSample(ts, energy_j, duration_us, !sync_now);
#endif
}
