#include "buttons.h"

#include <Arduino.h>

#include "../AuroraSettings.h"
#include "board.h"

namespace {

int pinFor(Button b) {
  switch (b) {
    case BTN_TOP_LEFT: return PIN_BTN_TOP_LEFT;
    case BTN_BOTTOM_LEFT: return PIN_BTN_BOTTOM_LEFT;
    case BTN_TOP_RIGHT: return PIN_BTN_TOP_RIGHT;
    case BTN_BOTTOM_RIGHT: return PIN_BTN_BOTTOM_RIGHT;
    default: return -1;
  }
}

}  // namespace

void buttonsBegin() {
  pinMode(PIN_BTN_TOP_LEFT, INPUT_PULLDOWN);
  pinMode(PIN_BTN_BOTTOM_LEFT, INPUT_PULLDOWN);
  pinMode(PIN_BTN_BOTTOM_RIGHT, INPUT_PULLDOWN);
  // Input-only pin: INPUT_PULLDOWN would be a silent no-op, the board
  // provides an external pulldown.
  pinMode(PIN_BTN_TOP_RIGHT, INPUT);
}

Button buttonsFromWakeMask(uint64_t mask) {
  if (mask & (1ULL << PIN_BTN_TOP_LEFT)) return BTN_TOP_LEFT;
  if (mask & (1ULL << PIN_BTN_BOTTOM_LEFT)) return BTN_BOTTOM_LEFT;
  if (mask & (1ULL << PIN_BTN_TOP_RIGHT)) return BTN_TOP_RIGHT;
  if (mask & (1ULL << PIN_BTN_BOTTOM_RIGHT)) return BTN_BOTTOM_RIGHT;
  return BTN_NONE;
}

bool buttonsAnyDown() {
  return digitalRead(PIN_BTN_TOP_LEFT) == HIGH ||
         digitalRead(PIN_BTN_BOTTOM_LEFT) == HIGH ||
         digitalRead(PIN_BTN_TOP_RIGHT) == HIGH ||
         digitalRead(PIN_BTN_BOTTOM_RIGHT) == HIGH;
}

bool buttonsWaitForHold(Button button) {
  int pin = pinFor(button);
  if (pin < 0) {
    return false;
  }

  delay(DEBOUNCE_MS);

  uint32_t start = millis();
  while (digitalRead(pin) == HIGH) {
    if (millis() - start >= HOLD_DURATION_MS) {
      return true;
    }
    delay(10);
  }
  return false;
}
