#pragma once

#include <stdint.h>

// Drop to 80 MHz for the normal wake path: most of it is spent waiting on the
// panel's BUSY line, and core current scales with clock. Only WiFi wants the
// full clock, so it finishes sooner and switches the radio off.
void powerSetLowClock();
void powerSetFullClock();

// Rate-limited: the reading is noisy and barely moves minute to minute.
uint8_t powerBatteryPercent(uint32_t now_utc);

// Always samples. The shutdown decision must act on the present voltage.
uint16_t powerBatteryMillivolts();

void powerVibratePulse();
void powerVibrateOff();
