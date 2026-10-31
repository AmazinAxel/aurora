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

// EXT1 ANY_HIGH on every button not currently held, with RTC pulldowns
// re-armed. Just before deep sleep. False if a held button was left out, which
// then needs a timer wake to arm it once released.
bool buttonsArmWake();

// Blocks until release or HOLD_DURATION_MS, whichever comes first. Not
// BTN_NONE.
bool buttonsWaitForHold(Button button);

// Sleeping with a button still down would immediately re-trigger the wake.
// Light sleeps rather than polls; gives up after timeout_ms.
void buttonsWaitForRelease(uint32_t timeout_ms);
