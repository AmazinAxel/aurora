#include "power.h"

#include <Arduino.h>

#include "../AuroraSettings.h"
#include "board.h"
#include "state.h"

namespace {

// The cell sits behind a 2:1 divider.
constexpr float kDividerRatio = 2.0f;
constexpr uint16_t kBatteryMinMv = 3300;
constexpr uint16_t kBatteryMaxMv = 4200;
constexpr uint32_t kSampleIntervalMin = 15;

uint16_t readBatteryMillivolts() {
  analogSetPinAttenuation(PIN_BATTERY_ADC, ADC_11db);

  // A single ESP32 ADC reading is noisy enough to swing the reported
  // percentage by several points between wakes.
  uint32_t total = 0;
  constexpr int kSamples = 8;
  for (int i = 0; i < kSamples; i++) {
    total += analogReadMilliVolts(PIN_BATTERY_ADC);
  }
  return (uint16_t)((total / kSamples) * kDividerRatio);
}

uint8_t millivoltsToPercent(uint16_t mv) {
  if (mv >= kBatteryMaxMv) return 100;
  if (mv <= kBatteryMinMv) return 0;
  return (uint8_t)(((uint32_t)(mv - kBatteryMinMv) * 100) /
                   (kBatteryMaxMv - kBatteryMinMv));
}

}  // namespace

void powerSetLowClock() { setCpuFrequencyMhz(80); }
void powerSetFullClock() { setCpuFrequencyMhz(240); }

uint16_t powerBatteryMillivolts() { return readBatteryMillivolts(); }

uint8_t powerBatteryPercent(uint32_t now_utc) {
  bool stale = g_state.battery_sampled_at == 0 ||
               now_utc < g_state.battery_sampled_at ||
               (now_utc - g_state.battery_sampled_at) >=
                   kSampleIntervalMin * 60;
  if (stale) {
    g_state.battery_percent = millivoltsToPercent(readBatteryMillivolts());
    g_state.battery_sampled_at = now_utc;
  }
  return g_state.battery_percent;
}

void powerVibratePulse() {
  pinMode(PIN_VIBRATE, OUTPUT);
  digitalWrite(PIN_VIBRATE, HIGH);
  delay(ALERT_PULSE_ON_MS);
  digitalWrite(PIN_VIBRATE, LOW);
}

void powerVibrateOff() {
  pinMode(PIN_VIBRATE, OUTPUT);
  digitalWrite(PIN_VIBRATE, LOW);
}
