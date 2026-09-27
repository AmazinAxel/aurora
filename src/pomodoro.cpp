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
  g_state.paused = false;
  g_state.phase_end_utc = now_utc + (uint32_t)minutes * 60UL;
  g_state.paused_remaining = 0;
  g_state.alert_started_utc = 0;
}

bool running() {
  return !g_state.paused &&
         (g_state.phase == POMO_FOCUS || g_state.phase == POMO_BREAK);
}

}  // namespace

void pomodoroStop() {
  g_state.phase = POMO_IDLE;
  g_state.paused = false;
  g_state.phase_end_utc = 0;
  g_state.paused_remaining = 0;
  g_state.alert_started_utc = 0;
}

void pomodoroCyclePreset(int8_t direction) {
  int count = (int)POMODORO_PRESET_COUNT;
  int next = (int)g_state.preset_index + (direction >= 0 ? 1 : -1);
  g_state.preset_index = (uint8_t)((next % count + count) % count);

  // Whatever was running was measured against the old durations, so carrying
  // it over would drain the bar against the wrong total.
  pomodoroStop();
}

void pomodoroStart(uint32_t now_utc) {
  beginPhase(POMO_FOCUS, now_utc, preset().focus_minutes);
}

void pomodoroTogglePause(uint32_t now_utc) {
  if (g_state.phase == POMO_IDLE) {
    pomodoroStart(now_utc);
  } else if (pomodoroAlerting()) {
    pomodoroAcknowledge(now_utc);
  } else if (g_state.paused) {
    // Stored as a span, not a timestamp, so resuming rebuilds the end instant
    // and the phase still runs its full remaining span.
    g_state.phase_end_utc = now_utc + g_state.paused_remaining;
    g_state.paused_remaining = 0;
    g_state.paused = false;
  } else {
    g_state.paused_remaining = g_state.phase_end_utc > now_utc
                                   ? g_state.phase_end_utc - now_utc
                                   : 0;
    g_state.paused = true;
  }
}

void pomodoroAcknowledge(uint32_t now_utc) {
  if (g_state.phase == POMO_ALERT_FOCUS_DONE) {
    beginPhase(POMO_BREAK, now_utc, preset().break_minutes);
  } else if (g_state.phase == POMO_ALERT_BREAK_DONE) {
    beginPhase(POMO_FOCUS, now_utc, preset().focus_minutes);
  }
}

void pomodoroTick(uint32_t now_utc) {
  if (!running() || now_utc < g_state.phase_end_utc) {
    return;
  }

  g_state.phase = g_state.phase == POMO_FOCUS ? POMO_ALERT_FOCUS_DONE
                                              : POMO_ALERT_BREAK_DONE;
  g_state.alert_started_utc = now_utc;
}

bool pomodoroAlerting() {
  return g_state.phase == POMO_ALERT_FOCUS_DONE ||
         g_state.phase == POMO_ALERT_BREAK_DONE;
}

bool pomodoroActive() { return g_state.phase != POMO_IDLE; }
bool pomodoroPaused() { return g_state.paused; }

uint16_t pomodoroFocusMinutes() { return preset().focus_minutes; }
uint16_t pomodoroBreakMinutes() { return preset().break_minutes; }

uint32_t pomodoroPhaseSeconds() {
  return (uint32_t)(g_state.phase == POMO_BREAK ? preset().break_minutes
                                                : preset().focus_minutes) *
         60UL;
}

bool pomodoroShouldBuzz(uint32_t now_utc) {
  if (!pomodoroAlerting() || g_state.alert_started_utc == 0) {
    return pomodoroAlerting();
  }
  return (now_utc - g_state.alert_started_utc) < ALERT_MAX_DURATION_SEC;
}

uint32_t pomodoroRemainingSeconds(uint32_t now_utc) {
  if (g_state.paused) {
    return g_state.paused_remaining;
  }
  if (running() && g_state.phase_end_utc > now_utc) {
    return g_state.phase_end_utc - now_utc;
  }
  return 0;
}

uint32_t pomodoroNextWakeUtc(uint32_t now_utc) {
  // Only the phase end itself. The bar needs no wakes of its own: it is
  // redrawn on the clock's minute tick, which halves wakes and panel
  // refreshes against stepping it on the phase's second-offset too.
  if (running()) {
    return g_state.phase_end_utc > now_utc ? g_state.phase_end_utc : now_utc;
  }

  if (pomodoroShouldBuzz(now_utc)) {
    // One wake per buzz. A full wake costs far more than the pulse itself, so
    // the gap is deliberately long rather than a tight motor duty cycle.
    return now_utc + ALERT_REPEAT_SEC;
  }

  return 0;
}
