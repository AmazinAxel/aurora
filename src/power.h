#pragma once

#include <stdint.h>

// Drop to 80 MHz for the normal wake path: most of it is spent waiting on the
// panel's BUSY line, and core current scales with clock. Only WiFi wants the
// full clock, so it finishes sooner and switches the radio off.
void powerSetLowClock();
void powerSetFullClock();

// Rate-limited: the reading is noisy and barely moves minute to minute.
uint8_t powerBatteryPercent(uint32_t now_utc);

// Sampled once per wake and cached; every caller sees the same reading.
uint16_t powerBatteryMillivolts();

enum ChargerEdge : uint8_t {
  CHARGER_NO_CHANGE = 0,
  CHARGER_PLUGGED,
  CHARGER_UNPLUGGED,
};

// Samples the rail and reports a plug/unplug transition, updating the stored
// reference. Call once per wake: it is edge-triggered, so an event that falls
// entirely between two calls is missed rather than deferred.
ChargerEdge powerCheckCharger();

// The motor runs while the panel refreshes, so a buzzing wake is not extended
// by the pulse; main times it against ALERT_PULSE_ON_MS.
void powerVibrateOn();
void powerVibrateOff();
