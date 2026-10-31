#pragma once

#include <stdint.h>

// The wake path runs at 40 MHz straight off the crystal (board_build.f_cpu),
// PLL off: most of it is spent waiting on the panel's BUSY line, and core
// current scales with clock. Only WiFi wants the full clock, so it finishes
// sooner and switches the radio off.
void powerSetLowClock();
void powerSetFullClock();

// Waiting on a pin with the CPU polling it costs current for nothing. Light
// sleep until it reads low or timeout_ms passes, whichever comes first; the
// caller re-reads the pin. Not while the motor runs: its pin is not
// guaranteed to hold through light sleep.
void powerLightSleepUntilLow(int pin, uint32_t timeout_ms);

// A fixed wait, light sleeping rather than idling. Same motor caveat.
void powerLightSleepMs(uint32_t ms);

// The motor-safe version: the CPU idles on the falling edge, no light sleep.
void powerIdleUntilLow(int pin, uint32_t timeout_ms);

// Motor off and its pin held low: as a floating input it would run the motor.
// The hold lasts through the next wake until the motor is next driven.
void powerPrepareDeepSleep();

// Both survive deep sleep, so only once per power-on. The accel needs I2C up.
void powerIsolateUnusedPins();
void powerSuspendAccel();

// Rate-limited: the reading is noisy and barely moves minute to minute.
uint8_t powerBatteryPercent(uint32_t now_utc);

// 0% on the display, and where the watch shuts down. Above the ESP32's ~3.0 V
// brownout threshold, so the watch parks itself deliberately rather than being
// reset mid-update -- a reset during a display write can leave the panel
// partially driven, which damages e-ink rather than merely ghosting it.
constexpr uint16_t kBatteryCutoffMv = 3400;

// Sampled once per wake and cached; every caller sees the same reading.
uint16_t powerBatteryMillivolts();

// Samples the rail and reports a plug-in, updating the stored reference. Call
// once per wake: it is edge-triggered, so an event that falls entirely between
// two calls is missed rather than deferred.
bool powerChargerPlugged();

// The motor runs while the panel refreshes, so a buzzing wake is not extended
// by the pulse; main times it against ALERT_PULSE_ON_MS.
void powerVibrate(bool on);
bool powerVibrating();
