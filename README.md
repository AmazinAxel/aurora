# Aurora

A Pomodoro e-ink timer for your wrist!

Featuring a very readable watchface with literally zero other UIs or menus. The entire firmware acts on a single display with only a sleep screen.

Press the top right or bottom right buttons to cycle through the Pomodoro presets: while you're choosing, the clock shows the focus minutes on top and the break minutes below. Hold the bottom right button to toggle daylight savings.

Use the bottom left button to pause/start the timer, or hold it to restart. While a timer runs, a play/pause mark sits in the side strip and a bar down the right edge drains from full to empty as the phase goes by. It will vibrate whenever its time to take a break or start working!

The sleep screen can save battery and appears when charging. The watch's RTC is always saved so you never lose the time! And if you do, it shows `--` until the next NTP sync, which runs automatically when you unplug the charger.

Everything is as optimized as possible for battery life and quick display refreshes with as few full refreshes as possible!

## Config

Put this in a `AuroraSettings.h` file in the project root!

```c
#pragma once

#include <stdint.h>

// Wifi for NTP syncing
#define WIFI_SSID ""
#define WIFI_PASSWORD ""

// NTP
#define NTP_TOTAL_TIMEOUT_MS 20000
#define NTP_SERVER "pool.ntp.org"

// Watch
#define MINUTES_AHEAD 2
#define TZ_OFFSET_MINUTES (-480) // UTC-8 PST
#define DST_OFFSET_MINUTES 60
#define PRESET_VISIBLE_SECONDS 6
#define FULL_REFRESH_INTERVAL 180
#define BATTERY_VISIBLE_BELOW 50

// Drift
#define DRIFT_MAX_PPM 500.0f
#define DRIFT_EMA_ALPHA 0.25f
#define DRIFT_MIN_INTERVAL_SEC 3600

// Buttons
#define HOLD_DURATION_MS 500
#define DEBOUNCE_MS 40

// Pomodoro
#define ALERT_PULSE_ON_MS 400
#define ALERT_REPEAT_SEC 5
#define ALERT_MAX_DURATION_SEC 300

struct PomodoroPreset {
  uint16_t focus_minutes;
  uint16_t break_minutes;
};

// Shown as two digits each, so at most 99 minutes.
static const PomodoroPreset POMODORO_PRESETS[] = {
    {25, 5},
    {30, 5},
    {50, 10},
    {90, 20},
};

#define POMODORO_PRESET_COUNT \
  (sizeof(POMODORO_PRESETS) / sizeof(POMODORO_PRESETS[0]))
```
