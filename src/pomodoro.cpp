#include "pomodoro.h"

#include "../AuroraSettings.h"
#include "state.h"

namespace {

const PomodoroPreset &preset() {
  uint8_t idx = g_state.preset_index;
  return POMODORO_PRESETS[idx < POMODORO_PRESET_COUNT ? idx : 0];
}

void beginPhase(uint8_t phase, uint32_t now_utc, uint16_t minutes) {
  g_state.phase = phase;
  g_state.phase_end_utc = now_utc + (uint32_t)minutes * 60UL;
  g_state.paused_remaining = 0;
  g_state.alert_started_utc = 0;
}

}  // namespace

void pomodoroStop() {
  g_state.phase = POMO_IDLE;
  g_state.phase_end_utc = 0;
  g_state.paused_remaining = 0;
  g_state.alert_started_utc = 0;
}

void pomodoroCyclePreset(int8_t direction) {
  int count = (int)POMODORO_PRESET_COUNT;
  int next = (int)g_state.preset_index + (direction >= 0 ? 1 : -1);
  g_state.preset_index = (uint8_t)((next % count + count) % count);

  // Whatever was running was measured against the old durations, so carrying
  // it over would show a countdown that contradicts the preset beside it.
  pomodoroStop();
}

void pomodoroStart(uint32_t now_utc) {
  beginPhase(POMO_FOCUS, now_utc, preset().focus_minutes);
}

void pomodoroReset(uint32_t now_utc) { pomodoroStart(now_utc); }

void pomodoroTogglePause(uint32_t now_utc) {
  switch (g_state.phase) {
    case POMO_IDLE:
      pomodoroStart(now_utc);
      break;

    case POMO_FOCUS:
    case POMO_BREAK:
      // Stored as a span, not a timestamp, so resuming rebuilds the end
      // instant and the countdown keeps its second-offset.
      g_state.paused_remaining = g_state.phase_end_utc > now_utc
                                     ? g_state.phase_end_utc - now_utc
                                     : 0;
      g_state.paused_from_phase = g_state.phase;
      g_state.phase = POMO_PAUSED;
      break;

    case POMO_PAUSED:
      g_state.phase =
          g_state.paused_from_phase == POMO_BREAK ? POMO_BREAK : POMO_FOCUS;
      g_state.phase_end_utc = now_utc + g_state.paused_remaining;
      g_state.paused_remaining = 0;
      break;

    default:
      pomodoroAcknowledge(now_utc);
      break;
  }
}

void pomodoroAcknowledge(uint32_t now_utc) {
  if (g_state.phase == POMO_ALERT_FOCUS_DONE) {
    beginPhase(POMO_BREAK, now_utc, preset().break_minutes);
  } else if (g_state.phase == POMO_ALERT_BREAK_DONE) {
    beginPhase(POMO_FOCUS, now_utc, preset().focus_minutes);
  }
}

bool pomodoroTick(uint32_t now_utc) {
  if ((g_state.phase != POMO_FOCUS && g_state.phase != POMO_BREAK) ||
      now_utc < g_state.phase_end_utc) {
    return false;
  }

  g_state.phase = g_state.phase == POMO_FOCUS ? POMO_ALERT_FOCUS_DONE
                                              : POMO_ALERT_BREAK_DONE;
  g_state.alert_started_utc = now_utc;
  return true;
}

bool pomodoroAlerting() {
  return g_state.phase == POMO_ALERT_FOCUS_DONE ||
         g_state.phase == POMO_ALERT_BREAK_DONE;
}

bool pomodoroActive() { return g_state.phase != POMO_IDLE; }

bool pomodoroRunning() {
  return g_state.phase == POMO_FOCUS || g_state.phase == POMO_BREAK;
}

uint16_t pomodoroFocusMinutes() { return preset().focus_minutes; }
uint16_t pomodoroBreakMinutes() { return preset().break_minutes; }

bool pomodoroShouldBuzz(uint32_t now_utc) {
  if (!pomodoroAlerting() || g_state.alert_started_utc == 0) {
    return pomodoroAlerting();
  }
  return (now_utc - g_state.alert_started_utc) < ALERT_MAX_DURATION_SEC;
}

uint16_t pomodoroRemainingMinutes(uint32_t now_utc) {
  uint32_t remaining;
  if (g_state.phase == POMO_PAUSED) {
    remaining = g_state.paused_remaining;
  } else if (pomodoroRunning()) {
    remaining =
        g_state.phase_end_utc > now_utc ? g_state.phase_end_utc - now_utc : 0;
  } else {
    return 0;
  }
  return (uint16_t)((remaining + 59) / 60);
}

uint32_t pomodoroNextWakeUtc(uint32_t now_utc) {
  if (pomodoroRunning()) {
    if (g_state.phase_end_utc <= now_utc) {
      return now_utc;
    }
    // Ticks land on the start's second-offset, not the wall-clock minute.
    uint32_t to_next = (g_state.phase_end_utc - now_utc) % 60;
    return now_utc + (to_next == 0 ? 60 : to_next);
  }

  if (pomodoroAlerting() && pomodoroShouldBuzz(now_utc)) {
    // One wake per buzz. A full wake costs far more than the pulse itself, so
    // the gap is deliberately long rather than a tight motor duty cycle.
    return now_utc + ALERT_REPEAT_SEC;
  }

  return 0;
}
