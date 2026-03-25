#include <Wire.h>
#include <Adafruit_INA228.h>
#include "pb_encode.h"
#include "power_sample.pb.h"

Adafruit_INA228 ina228;

static constexpr uint8_t INA228_I2C_ADDR = INA228_I2CADDR_DEFAULT;
static constexpr float SHUNT_RESISTOR_OHMS = 0.1f; // R100
static constexpr float MAX_EXPECTED_CURRENT_A = 10.0f;

static constexpr int INA228_SDA_PIN = 2;
static constexpr int INA228_SCL_PIN = 1;

static constexpr int IS_INFERENCING_PIN = 3;

// Encode buffer for protobuf payload.
static uint8_t pb_buf[PowerSample_size];

// --- ISR: capture pin state at the moment of the edge ---
static volatile bool edge_detected = false;
static volatile bool edge_pin_high = false;

static void IRAM_ATTR onSyncEdge() {
  edge_pin_high = digitalRead(IS_INFERENCING_PIN) == HIGH;
  edge_detected = true;
}

// Window start timestamp (set after each accumulator reset).
static uint32_t window_start_us = 0;

static void sendSample(uint32_t ts, float avg_mw, uint32_t duration_us,
                       bool is_inference) {
  PowerSample sample = PowerSample_init_zero;
  sample.timestamp_us = ts;
  sample.avg_mw = (avg_mw >= 0.0f) ? (uint32_t)(avg_mw + 0.5f) : 0;
  sample.duration_us = duration_us;
  sample.is_inference = is_inference;

  pb_ostream_t stream = pb_ostream_from_buffer(pb_buf, sizeof(pb_buf));
  if (!pb_encode(&stream, PowerSample_fields, &sample)) {
    return;
  }

  uint32_t len = (uint32_t)stream.bytes_written;
  uint8_t prefix[4] = {
      (uint8_t)(len),
      (uint8_t)(len >> 8),
      (uint8_t)(len >> 16),
      (uint8_t)(len >> 24),
  };
  Serial.write(prefix, 4);
  Serial.write(pb_buf, len);
}

void setup() {
  Serial.begin(921600);
  delay(200);

  Wire.begin(INA228_SDA_PIN, INA228_SCL_PIN);
  Wire.setClock(400000);
  pinMode(IS_INFERENCING_PIN, INPUT_PULLDOWN);

  Serial.println("# INA228 monitor starting...");
  while (true) {
    if (!ina228.begin(INA228_I2C_ADDR, &Wire)) {
      Serial.println("ERROR: INA228 not found.");
    } else {
      break;
    }
    delay(1000);
  }

  ina228.setShunt(SHUNT_RESISTOR_OHMS, MAX_EXPECTED_CURRENT_A);

  // Fast conversion: 4x averaging, 50us per conversion -> ~400us cycle.
  // The hardware accumulator integrates every conversion internally.
  ina228.setAveragingCount(INA228_COUNT_4);
  ina228.setVoltageConversionTime(INA228_TIME_50_us);
  ina228.setCurrentConversionTime(INA228_TIME_50_us);
  ina228.resetAccumulators();

  window_start_us = micros();
  attachInterrupt(digitalPinToInterrupt(IS_INFERENCING_PIN), onSyncEdge, CHANGE);
}

void loop() {
  if (!edge_detected) {
    return;
  }
  edge_detected = false;

  const uint32_t ts = micros();
  const bool sync_now = edge_pin_high;

  // Read accumulated energy for the window that just ended.
  float energy_j = ina228.readEnergy();
  uint32_t duration_us = ts - window_start_us;

  // Compute average power (mW) over the window.
  float avg_mw = 0.0f;
  if (duration_us > 0 && energy_j >= 0.0f) {
    avg_mw = (energy_j / ((float)duration_us * 1e-6f)) * 1000.0f;
  }

  // Reset accumulator for the next window.
  ina228.resetAccumulators();
  window_start_us = ts;

  // Rising edge (sync_now=true): idle window just ended.
  // Falling edge (sync_now=false): inference window just ended.
  sendSample(ts, avg_mw, duration_us, !sync_now);
}
