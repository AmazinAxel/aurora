#pragma once

#include <stdint.h>

// The countdown is locked to the instant the pomodoro started, not to a
// wall-clock minute: a 30 minute focus begun at 14:32:47 shows "30" until
// 15:02:47 and ends exactly then. Phases run continuously -- a phase ends,
// the watch buzzes until acknowledged, and that press starts the next.

void pomodoroCyclePreset(int8_t direction);
void pomodoroStart(uint32_t now_utc);
void pomodoroStop();
void pomodoroReset(uint32_t now_utc);
void pomodoroTogglePause(uint32_t now_utc);

// Advance a finished phase into the next one.
void pomodoroAcknowledge(uint32_t now_utc);

// Roll an expired phase into its alert state. True if the screen must redraw.
bool pomodoroTick(uint32_t now_utc);

bool pomodoroAlerting();
bool pomodoroActive();
bool pomodoroRunning();

// False once the alert has gone unacknowledged past ALERT_MAX_DURATION_SEC.
bool pomodoroShouldBuzz(uint32_t now_utc);

// Rounded up, so the display holds the full starting figure for a whole
// minute.
uint16_t pomodoroRemainingMinutes(uint32_t now_utc);

uint16_t pomodoroFocusMinutes();
uint16_t pomodoroBreakMinutes();

// Next instant the pomodoro needs the CPU awake, or 0 if it does not.
uint32_t pomodoroNextWakeUtc(uint32_t now_utc);
