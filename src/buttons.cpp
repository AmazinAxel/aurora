#include "buttons.h"

#include <Arduino.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>

#include "../AuroraSettings.h"
#include "board.h"
#include "power.h"

namespace {

// In Button order, from BTN_TOP_LEFT.
constexpr int kPins[] = {PIN_BTN_TOP_LEFT, PIN_BTN_BOTTOM_LEFT,
                         PIN_BTN_TOP_RIGHT, PIN_BTN_BOTTOM_RIGHT};

// GPIO 35 is input-only with no internal pulldown; it relies on the board's
// external one. Still RTC-capable, so it takes part in EXT1 wake normally.
bool hasInternalPulldown(int pin) { return pin != PIN_BTN_TOP_RIGHT; }

int firstDownPin() {
  for (int pin : kPins) {
    if (digitalRead(pin) == HIGH) {
      return pin;
    }
  }
  return -1;
}

}  // namespace

void buttonsBegin() {
  for (int pin : kPins) {
    pinMode(pin, hasInternalPulldown(pin) ? INPUT_PULLDOWN : INPUT);
  }
}

bool buttonsArmWake() {
  uint64_t mask = 0;
  bool all_armed = true;
  for (int pin : kPins) {
    // ANY_HIGH on a pin already high would wake straight back up, forever.
    if (digitalRead(pin) == HIGH) {
      all_armed = false;
      continue;
    }
    mask |= 1ULL << pin;
    // Digital pulls are switched off in deep sleep, so the pins would float
    // and could spuriously trigger the ANY_HIGH wake.
    if (hasInternalPulldown(pin)) {
      rtc_gpio_pulldown_en((gpio_num_t)pin);
    }
  }
  if (mask != 0) {
    esp_sleep_enable_ext1_wakeup(mask, ESP_EXT1_WAKEUP_ANY_HIGH);
  }
  return all_armed;
}

Button buttonsFromWakeMask(uint64_t mask) {
  for (uint8_t i = 0; i < sizeof(kPins) / sizeof(kPins[0]); i++) {
    if (mask & (1ULL << kPins[i])) {
      return (Button)(BTN_TOP_LEFT + i);
    }
  }
  return BTN_NONE;
}

void buttonsWaitForRelease(uint32_t timeout_ms) {
  uint32_t start = millis();
  for (int pin; (pin = firstDownPin()) >= 0;) {
    uint32_t elapsed = millis() - start;
    if (elapsed >= timeout_ms) {
      return;
    }
    powerLightSleepUntilLow(pin, timeout_ms - elapsed);
    // Contacts bounce on release; let them settle before re-checking, or a
    // bounce would re-trigger the ANY_HIGH wake the moment the watch sleeps.
    powerLightSleepMs(DEBOUNCE_MS);
  }
}

bool buttonsWaitForHold(Button button) {
  int pin = kPins[button - BTN_TOP_LEFT];

  // No debounce: booting from the press to here already outlasts the bounce.
  uint32_t start = millis();
  while (digitalRead(pin) == HIGH) {
    uint32_t elapsed = millis() - start;
    if (elapsed >= HOLD_DURATION_MS) {
      return true;
    }
    powerLightSleepUntilLow(pin, HOLD_DURATION_MS - elapsed);
  }
  return false;
}
