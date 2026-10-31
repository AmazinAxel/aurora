#include "power.h"

#include <Arduino.h>
#include <Wire.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>

#include "board.h"
#include "state.h"

namespace {

// The cell sits behind a 2:1 divider.
constexpr float kDividerRatio = 2.0f;
constexpr uint16_t kBatteryMaxMv = 4200;
constexpr uint32_t kSampleIntervalMin = 15;

// One reading per wake, shared by the shutdown check, charger detection and
// the displayed percentage. Plain static RAM, so deep sleep clears it and the
// next wake samples afresh.
uint16_t g_wake_mv = 0;

bool g_vibrating = false;

SemaphoreHandle_t g_pin_low = nullptr;

void IRAM_ATTR onPinLow() {
  BaseType_t woken = pdFALSE;
  xSemaphoreGiveFromISR(g_pin_low, &woken);
  if (woken) {
    portYIELD_FROM_ISR();
  }
}

uint8_t millivoltsToPercent(uint16_t mv) {
  if (mv >= kBatteryMaxMv) return 100;
  if (mv <= kBatteryCutoffMv) return 0;
  return (uint8_t)(((uint32_t)(mv - kBatteryCutoffMv) * 100) /
                   (kBatteryMaxMv - kBatteryCutoffMv));
}

}  // namespace

void powerSetLowClock() { setCpuFrequencyMhz(F_CPU / 1000000); }
void powerSetFullClock() { setCpuFrequencyMhz(240); }

void powerLightSleepMs(uint32_t ms) {
  esp_sleep_enable_timer_wakeup(ms * 1000ULL);
  esp_light_sleep_start();
}

void powerLightSleepUntilLow(int pin, uint32_t timeout_ms) {
  gpio_wakeup_enable((gpio_num_t)pin, GPIO_INTR_LOW_LEVEL);
  esp_sleep_enable_gpio_wakeup();
  esp_sleep_enable_timer_wakeup(timeout_ms * 1000ULL);
  esp_light_sleep_start();
  // Left armed, a pin that idles low would cut every later light sleep short.
  gpio_wakeup_disable((gpio_num_t)pin);
}

void powerIdleUntilLow(int pin, uint32_t timeout_ms) {
  if (g_pin_low == nullptr) {
    g_pin_low = xSemaphoreCreateBinary();
  }
  xSemaphoreTake(g_pin_low, 0);
  attachInterrupt(digitalPinToInterrupt(pin), onPinLow, FALLING);
  // Checked after arming, so an edge between the two cannot be missed.
  if (digitalRead(pin) == HIGH) {
    xSemaphoreTake(g_pin_low, pdMS_TO_TICKS(timeout_ms));
  }
  detachInterrupt(digitalPinToInterrupt(pin));
}

void powerPrepareDeepSleep() {
  powerVibrate(false);
  rtc_gpio_hold_en((gpio_num_t)PIN_VIBRATE);
}

void powerIsolateUnusedPins() {
  // None of these are wired to anything the firmware uses; left connected
  // they leak.
  for (gpio_num_t pin : {GPIO_NUM_12, GPIO_NUM_14, GPIO_NUM_27, GPIO_NUM_32,
                         GPIO_NUM_33}) {
    rtc_gpio_isolate(pin);
  }
}

void powerSuspendAccel() {
  // Only a BMA423 (chip id 0x13), the part official Watchy boards carry: the
  // accel here is unidentified, and these registers mean nothing elsewhere.
  Wire.beginTransmission(ACCEL_I2C_ADDR);
  Wire.write((uint8_t)0x00);
  if (Wire.endTransmission() != 0 ||
      Wire.requestFrom((int)ACCEL_I2C_ADDR, 1) != 1 || Wire.read() != 0x13) {
    return;
  }
  Wire.beginTransmission(ACCEL_I2C_ADDR);
  Wire.write((uint8_t)0x7D);  // PWR_CTRL: accel off
  Wire.write((uint8_t)0x00);
  Wire.endTransmission();
  Wire.beginTransmission(ACCEL_I2C_ADDR);
  Wire.write((uint8_t)0x7C);  // PWR_CONF: advanced power save
  Wire.write((uint8_t)0x03);
  Wire.endTransmission();
}

uint16_t powerBatteryMillivolts() {
  if (g_wake_mv != 0) {
    return g_wake_mv;
  }

  analogSetPinAttenuation(PIN_BATTERY_ADC, ADC_11db);

  // A single ESP32 ADC reading is noisy enough to swing the reported
  // percentage by several points between wakes.
  uint32_t total = 0;
  constexpr int kSamples = 4;
  for (int i = 0; i < kSamples; i++) {
    total += analogReadMilliVolts(PIN_BATTERY_ADC);
  }
  g_wake_mv = (uint16_t)((total / kSamples) * kDividerRatio);
  return g_wake_mv;
}

uint8_t powerBatteryPercent(uint32_t now_utc) {
  bool stale = g_state.battery_sampled_at == 0 ||
               now_utc < g_state.battery_sampled_at ||
               (now_utc - g_state.battery_sampled_at) >=
                   kSampleIntervalMin * 60;
  if (stale) {
    g_state.battery_percent = millivoltsToPercent(powerBatteryMillivolts());
    g_state.battery_sampled_at = now_utc;
  }
  return g_state.battery_percent;
}

bool powerChargerPlugged() {
  uint16_t mv = powerBatteryMillivolts();
  uint16_t last = g_state.last_vbat_mv;
  g_state.last_vbat_mv = mv;

  // Nothing to compare against on a cold boot.
  if (last == 0) return false;

  // Measured step is ~45 mV against ~4 mV of sample-to-sample jitter, so this
  // sits well clear of noise. A cell on battery only falls between wakes, and
  // charging itself climbs far slower than this per minute tick.
  constexpr uint16_t kEdgeMv = 25;

  return mv > last + kEdgeMv;
}

// Always re-asserted as an output: as an input the pin runs the motor. Level
// first, then the release, so the pad never glitches out of its held low.
void powerVibrate(bool on) {
  pinMode(PIN_VIBRATE, OUTPUT);
  digitalWrite(PIN_VIBRATE, on ? HIGH : LOW);
  rtc_gpio_hold_dis((gpio_num_t)PIN_VIBRATE);
  g_vibrating = on;
}

bool powerVibrating() { return g_vibrating; }
