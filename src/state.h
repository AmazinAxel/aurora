#pragma once

#include <Arduino.h>

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
  POMO_ALERT_FOCUS_DONE,
  POMO_ALERT_BREAK_DONE,
};

// sizeof(WatchfaceContent), checked in display.cpp.
#define DRAWN_CONTENT_BYTES 18

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
  // locked to the second the phase started rather than the wall clock. A
  // paused phase keeps its value; paused_remaining holds the span left.
  uint8_t preset_index;
  uint8_t phase;
  bool paused;
  uint32_t phase_end_utc;
  uint32_t paused_remaining;
  uint32_t alert_started_utc;

  uint8_t screen;
  // While in the future, the clock rows show the preset instead of the time.
  uint32_t preset_shown_until;

  // Partial refreshes since the last full one, and what the panel currently
  // shows, so an unchanged frame skips the update and a changed one writes
  // only the region that differs. Invalid while the panel holds the cover.
  uint16_t partial_count;
  bool drawn_valid;
  uint8_t drawn_content[DRAWN_CONTENT_BYTES];

  uint8_t battery_percent;
  uint32_t battery_sampled_at;
  bool battery_shutdown;

  // Charger detection. This board routes VBUS nowhere the MCU can read -- a
  // sweep of every RTC-capable GPIO found none that differ plugged vs
  // unplugged -- so the only signal left is the battery rail, which sits
  // ~45 mV higher while a charger holds it up. That offset is not a fixed
  // voltage (a resting cell and a charging one can read the same absolute
  // value at different charge levels), so detection compares consecutive
  // samples and looks for the step rather than a threshold.
  uint16_t last_vbat_mv;

  // Channel of the last successful connect, so the next one skips the full
  // scan. 0 scans everything.
  uint8_t wifi_channel;

  // The implausible step the last sync asked for, 0 if it asked for none.
  int32_t rejected_ntp_step;
};

#define STATE_MAGIC 0x46574835u

extern RTC_DATA_ATTR PersistedState g_state;

// True if RTC memory was cold and defaults were just applied.
bool stateInitIfCold();
