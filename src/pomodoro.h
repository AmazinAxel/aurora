#pragma once

#include <stdint.h>

// The phase is locked to the instant it started, not to a wall-clock minute:
// a 30 minute focus begun at 14:32:47 ends exactly at 15:02:47. Phases run
// continuously -- a phase ends, the watch buzzes until acknowledged, and that
// press starts the next.

void pomodoroCyclePreset(int8_t direction);
void pomodoroStart(uint32_t now_utc);
void pomodoroStop();
void pomodoroTogglePause(uint32_t now_utc);

// Advance a finished phase into the next one.
void pomodoroAcknowledge(uint32_t now_utc);

// Roll an expired phase into its alert state.
void pomodoroTick(uint32_t now_utc);

bool pomodoroAlerting();
bool pomodoroActive();
bool pomodoroPaused();

// False once the alert has gone unacknowledged past ALERT_MAX_DURATION_SEC.
bool pomodoroShouldBuzz(uint32_t now_utc);

// Seconds left in the current phase, frozen while paused. 0 when idle or
// alerting.
uint32_t pomodoroRemainingSeconds(uint32_t now_utc);

// Length of the current phase, which is what the bar is a fraction of.
uint32_t pomodoroPhaseSeconds();

uint16_t pomodoroFocusMinutes();
uint16_t pomodoroBreakMinutes();

// Next instant the pomodoro needs the CPU awake, or 0 if it does not.
uint32_t pomodoroNextWakeUtc(uint32_t now_utc);
