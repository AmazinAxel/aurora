#pragma once

#include <stdint.h>

enum Button : uint8_t {
  BTN_NONE = 0,
  BTN_TOP_LEFT,
  BTN_BOTTOM_LEFT,
  BTN_TOP_RIGHT,
  BTN_BOTTOM_RIGHT,
};

void buttonsBegin();
Button buttonsFromWakeMask(uint64_t mask);

// Blocks until release or HOLD_DURATION_MS, whichever comes first.
bool buttonsWaitForHold(Button button);

// Sleeping with a button still down would immediately re-trigger the wake.
bool buttonsAnyDown();
