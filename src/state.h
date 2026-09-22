#pragma once

#include <Arduino.h>
#include <stdint.h>
#include <string.h>

// Persisted in RTC slow memory: survives deep sleep at no power cost, unlike
// NVS which would wear flash on every minute tick. Does NOT survive power
// loss, so every field must have a sane zero value.

enum Screen : uint8_t {
  SCREEN_WATCHFACE = 0,
  SCREEN_COVER,
};

enum PomodoroPhase : uint8_t {
  POMO_IDLE = 0,
  POMO_FOCUS,
  POMO_BREAK,
  POMO_PAUSED,
  POMO_ALERT_FOCUS_DONE,
  POMO_ALERT_BREAK_DONE,
};

enum NtpStatus : uint8_t {
  NTP_NONE = 0,
  NTP_OK,
  NTP_FAILED,
};

struct PersistedState {
  uint32_t magic;

  // Timebase. last_ntp_utc/last_ntp_rtc pin true UTC to the chip's own scale
  // at the last sync; drift_ppm corrects the crystal error accumulated since.
  uint32_t last_ntp_utc;
  uint32_t last_ntp_rtc;
  float drift_ppm;
  bool drift_valid;
  bool dst_active;

  // Pomodoro. phase_end_utc is an absolute instant, so the countdown stays
  // locked to the second the phase started rather than the wall clock.
  uint8_t preset_index;
  uint8_t phase;
  uint32_t phase_end_utc;
  uint32_t paused_remaining;
  uint8_t paused_from_phase;
  uint32_t alert_started_utc;

  uint8_t screen;
  uint8_t ntp_status;
  uint32_t ntp_status_until;
  uint32_t preset_shown_until;

  // Partial refreshes since the last full one, and a hash of what the panel
  // currently shows so an unchanged frame can skip the update entirely.
  uint16_t partial_count;
  uint32_t drawn_hash;

  uint8_t battery_percent;
  uint32_t battery_sampled_at;
  bool battery_shutdown;
};

#define STATE_MAGIC 0x46574832u

extern RTC_DATA_ATTR PersistedState g_state;

// True if RTC memory was cold and defaults were just applied.
bool stateInitIfCold();
