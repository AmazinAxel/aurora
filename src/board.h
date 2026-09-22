#pragma once

#include <stdint.h>

// Physical facts about this board, confirmed with probe/probe.cpp. Tunable
// preferences live in AuroraSettings.h instead.
//
// Probe findings: I2C 0x18 accelerometer (unused), 0x51 PCF8563 RTC.
// GPIO 33 oscillates at ~30 Hz -- it is the 32.768 kHz RTC crystal line, not
// a button, and must stay out of the wake mask. GPIO 13 drives the vibration
// motor; configuring it as an input runs the motor.

// Buttons are active HIGH, so they need pulldowns and EXT1 ANY_HIGH -- which
// is also the only usable multi-pin deep-sleep mode on ESP32 classic, since
// ANY_LOW does not exist until the S3.
#define PIN_BTN_TOP_LEFT 25
#define PIN_BTN_BOTTOM_LEFT 26
#define PIN_BTN_TOP_RIGHT 35
#define PIN_BTN_BOTTOM_RIGHT 4

// GPIO 35 is input-only with no internal pulldown; it relies on the board's
// external one. Still RTC-capable, so it takes part in EXT1 wake normally.
#define BUTTON_WAKE_MASK                                       \
  ((1ULL << PIN_BTN_TOP_LEFT) | (1ULL << PIN_BTN_BOTTOM_LEFT) | \
   (1ULL << PIN_BTN_TOP_RIGHT) | (1ULL << PIN_BTN_BOTTOM_RIGHT))

#define PIN_I2C_SDA 21
#define PIN_I2C_SCL 22
#define RTC_I2C_ADDR 0x51

#define PIN_VIBRATE 13
#define PIN_BATTERY_ADC 34

// GDEH0154D67, 200x200.
#define PIN_EPD_CS 5
#define PIN_EPD_DC 10
#define PIN_EPD_RST 9
#define PIN_EPD_BUSY 19
