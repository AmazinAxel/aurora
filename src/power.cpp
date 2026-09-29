#include "power.h"

#include <Arduino.h>

#include "board.h"
#include "state.h"

namespace {

// The cell sits behind a 2:1 divider.
constexpr float kDividerRatio = 2.0f;
constexpr uint16_t kBatteryMinMv = 3300;
constexpr uint16_t kBatteryMaxMv = 4200;
constexpr uint32_t kSampleIntervalMin = 15;

// One reading per wake, shared by the shutdown check, charger detection and
// the displayed percentage. Plain static RAM, so deep sleep clears it and the
// next wake samples afresh.
uint16_t g_wake_mv = 0;

uint16_t readBatteryMillivolts() {
  if (g_wake_mv != 0) {
    return g_wake_mv;
  }

  analogSetPinAttenuation(PIN_BATTERY_ADC, ADC_11db);

  // A single ESP32 ADC reading is noisy enough to swing the reported
  // percentage by several points between wakes.
  uint32_t total = 0;
  constexpr int kSamples = 8;
  for (int i = 0; i < kSamples; i++) {
    total += analogReadMilliVolts(PIN_BATTERY_ADC);
  }
  g_wake_mv = (uint16_t)((total / kSamples) * kDividerRatio);
  return g_wake_mv;
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

ChargerEdge powerCheckCharger() {
  uint16_t mv = readBatteryMillivolts();
  uint16_t last = g_state.last_vbat_mv;
  g_state.last_vbat_mv = mv;

  // Nothing to compare against on a cold boot. Assume unplugged: guessing
  // wrong here only costs one missed sync, whereas assuming plugged would
  // fire a spurious unplug event on the next wake.
  if (last == 0) return CHARGER_NO_CHANGE;

  // Measured step is ~45 mV against ~4 mV of sample-to-sample jitter, so this
  // sits well clear of noise. Natural discharge moves far slower than this
  // per wake, so ordinary drain cannot look like an unplug.
  constexpr uint16_t kEdgeMv = 25;

  if (!g_state.charger_present && mv > last + kEdgeMv) {
    g_state.charger_present = true;
    return CHARGER_PLUGGED;
  }
  if (g_state.charger_present && last > mv + kEdgeMv) {
    g_state.charger_present = false;
    return CHARGER_UNPLUGGED;
  }
  return CHARGER_NO_CHANGE;
}

namespace {
bool g_vibrating = false;
}

void powerVibrateOn() {
  pinMode(PIN_VIBRATE, OUTPUT);
  digitalWrite(PIN_VIBRATE, HIGH);
  g_vibrating = true;
}

void powerVibrateOff() {
  pinMode(PIN_VIBRATE, OUTPUT);
  digitalWrite(PIN_VIBRATE, LOW);
  g_vibrating = false;
}

bool powerVibrating() { return g_vibrating; }
