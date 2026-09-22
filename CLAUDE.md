# Aurora

Pomodoro e-ink watch firmware for a USB-C Watchy-class ESP32 board
(AliExpress clone, not official SQFMI). Built for battery life and panel
longevity above all else.

## Hardware

Identified with a throwaway I2C-scan/button sketch, since removed. The
findings below are the record; nothing re-derives them.

| Part | Detail |
|---|---|
| MCU | ESP32 classic (not S3 — matters for wake modes) |
| Panel | GDEH0154D67, 200x200 mono, SPI |
| RTC | PCF8563 @ I2C 0x51 |
| Accelerometer | 0x18, unused |
| Buttons | **active HIGH**: TL=25, BL=26, TR=35, BR=4 |
| Vibration motor | GPIO 13 (output — driving it as input runs the motor) |
| Battery sense | GPIO 34, behind a 2:1 divider |

Traps that cost real debugging time:

- **GPIO 33 is the 32.768 kHz RTC crystal line**, not a button. It oscillates
  at ~30 Hz. Including it in the wake mask wakes the watch constantly.
- **GPIO 35 is input-only, no internal pulldown.** Relies on the board's
  external one; `INPUT_PULLDOWN` on it is a silent no-op.
- **ESP32 classic EXT1 has only `ANY_HIGH` and `ALL_LOW`.** There is no
  `ANY_LOW` until the S3, so active-low buttons could not wake this board at
  all. The buttons being active-high is what makes `ANY_HIGH` correct.
- RTC-domain pulldowns must be re-armed before sleep (`rtc_gpio_pulldown_en`);
  digital pulls switch off and floating pins spuriously trigger `ANY_HIGH`.
- `adc_power_release()` aborts — `analogReadMilliVolts` manages the SAR power
  refcount itself. Calling it caused a boot loop.

## Architecture

No Arduino `loop()`. Everything runs in `setup()`: a wake does its work and
deep sleeps again. Deep sleep is the only idle state.

```
wake (timer or EXT1 button)
  -> powerSetLowClock()            80 MHz; the wake is mostly BUSY-line wait
  -> stateInitIfCold()             RTC_DATA_ATTR struct, zeroed on cold boot
  -> rtcBegin() / timeNowUtc()
  -> handleBatteryShutdown()       before anything touches the panel
  -> button handler OR pomodoroTick()
  -> buzz if a phase ended unacknowledged
  -> displayWatchface()            skipped entirely if content hash matches
  -> wait for button release
  -> sleepNow(nextWakeDelay())
```

| File | Role |
|---|---|
| `src/main.cpp` | wake dispatch, button map, sleep scheduling |
| `src/state.h/.cpp` | `RTC_DATA_ATTR` persisted struct |
| `src/rtc.cpp` | PCF8563 over raw `Wire`, no RTC library |
| `src/timekeeping.cpp` | drift model, TZ/DST/minutes-ahead |
| `src/display.cpp` | all panel drawing + refresh policy |
| `src/pomodoro.cpp` | phase state machine |
| `src/power.cpp` | CPU clock, battery ADC, motor |
| `src/net.cpp` | NTP with guaranteed radio teardown |
| `src/buttons.cpp` | debounce, hold detection |
| `src/board.h` | pins (physical facts) |
| `tools/` | host-side asset + font generation (build-time only) |
| `AuroraSettings.h` | user preferences (project root) |

## Timekeeping

The RTC holds **true UTC**. Timezone, DST and `MINUTES_AHEAD` are applied only
on the way to the display, so the stored timebase stays comparable with NTP.

Drift is self-calibrating: each successful sync compares the chip's own
elapsed time against true elapsed time to get ppm, EMA-smoothed
(`DRIFT_EMA_ALPHA`), rejected beyond `DRIFT_MAX_PPM` or under
`DRIFT_MIN_INTERVAL_SEC` (short intervals are NTP round-trip noise, not drift).

**Wakes use the ESP32 timer, not the RTC alarm.** An earlier version armed the
PCF8563 minute alarm but never enabled EXT0 on its interrupt line, so the alarm
could never fire — the clock only updated on button presses. The RTC alarm path
is gone; the chip is purely a timebase now.

## Display and ink policy

`displayWatchface` / `displayCover` are the only entry points, so the policy
cannot be bypassed.

- Content is resolved into a struct, FNV-1a hashed, and compared with
  `g_state.drawn_hash`. Unchanged → **no panel drive at all**, and the panel is
  never even initialised (`ensureInitialised` is lazy). Largest single saving.
- Partial refresh by default. Full refresh on cold boot, on the cover, and
  every `FULL_REFRESH_INTERVAL` (180) partials.
- Ghosting here is reversible trapped charge, not damage — but the panel spec
  wants a full refresh at least every ~180 partials or it stops clearing
  easily. Entering the cover is a full refresh because the watch then dwells on
  that image for hours; coming *out* is partial.
- `hibernate()` before every sleep.

### Layout

Everything but the clock is in one 23 px strip down the left edge; the clock
gets the rest. **Width is the binding constraint on a 200 px square panel.**

```
┌────┬──────────────┐
│ 0  │              │   strip, top to bottom:
│ 9  │     14       │     month digits (stacked, one per row)
│    │              │     day digits
│ 1  │              │     weekday (rotated, FONT_WEEKDAY 9pt)
│ 5  │     32       │     rule + battery (only below 50%)
│ W  │              │
│ E  │              │   clock: 2 rows idle, 3 rows when the
│ D  │              │   bottom row is occupied
│────│              │
│ 4  │              │
│ 9  │              │
└────┴──────────────┘
```

Bottom row priority: NTP message > preset preview > active timer > nothing.
Whatever wins shrinks the clock from 2 rows to 3.

Typography gotchas, all learned the hard way:

- Digit columns centre on **advance width, not ink box**. A `1` is 6 px of ink
  with its stem hard right; centring the box pushes the stem — what the eye
  tracks down a column — off axis.
- `drawRotated` takes the **leftmost column, not the baseline**. Under rotation
  glyphs extend left of the baseline by their ascent; passing a small x
  directly clips the text off-panel.
- The rotated weekday draws **last**, after every upright item, because it
  flips global rotation.
- Rotated text's **cap height becomes its width** — hence a separate smaller
  `FONT_WEEKDAY`.

## Fonts

Barlow Semi Condensed (SIL OFL). Condensed on purpose: the clock is bounded by
two-digit width, so a narrow face buys height.

Generated by `tools/make_fonts.sh <ttf-dir>`, committed to `src/assets/`. Time
fonts are **digits only**; `Preset.h` also carries `/`. `tools/subset_font.py`
runs at build time and trims `UI.h` to the drawn characters.

**Adding on-screen text means adding its characters to `tools/subset_font.py`,
or it renders blank.**

Adafruit GFX has **no text alignment API** — only `getTextBounds`. Every
alignment is computed.

## Power

- 80 MHz for the whole wake path; 240 MHz only inside `netSyncNtp`, because the
  radio dwarfs the core and finishing sooner wins.
- `radioOff()` runs on every NTP exit path including failures.
- Battery sampled every 15 min, not per wake (the reading is noisy and barely
  moves).
- Below 3400 mV the watch parks on the cover and wakes on a 10 min timer only —
  buttons are not a wake source there. Recovers at 3600 mV (hysteresis stops
  flapping). Cutoff sits above the ~3.0 V brownout so it shuts down
  deliberately rather than being reset mid-panel-write.

## Controls

| | Tap | Hold (500 ms) |
|---|---|---|
| Top-left | Stop + clear pomodoro | Sleep to cover |
| Bottom-left | Pause / start | Reset + start |
| Top-right | Next preset | NTP sync |
| Bottom-right | Previous preset | Toggle DST |

On the cover, only top-left responds (power button); the rest are swallowed.
While buzzing, **any** press acknowledges and starts the next phase.

Changing preset stops a running timer — its remaining time was measured
against the old durations.

## Pomodoro

The countdown is locked to the **start instant**, not the wall clock. A 30 min
focus begun at 14:32:47 reads "30" until 15:02:47 and ends exactly then. Hence
`(remaining + 59) / 60` and wakes on the start's second-offset, which is why
`nextWakeDelay` takes the minimum of two unaligned schedules.

Phases chain continuously: focus → buzz until acknowledged → break → buzz →
focus. Buzzing is pulsed (`ALERT_REPEAT_SEC`, one wake per buzz — a wake costs
far more than the pulse) and gives up after `ALERT_MAX_DURATION_SEC`, though
the phase still waits for a press.

## Build

```
pio run -t upload
```

The sleep image is generated from `cover.png` (200x200) in the project root by
`tools/gen_assets.py`. The generated `src/assets/cover.h` is committed, so the
build works without the PNG present and only regenerates when it changes.

~792 KB flash (25%), ~50 KB RAM. Dominated by the WiFi/lwIP stack that NTP
requires; the app itself is small.

Not worth revisiting, already measured:

- `-Wl,--gc-sections`, `-fno-rtti`: zero bytes, Arduino core applies them.
- Replacing `snprintf` with hand-rolled integer formatting saved **no flash** —
  `_vfprintf_r` is pulled in by ESP-IDF regardless. Kept only because it avoids
  runtime formatting work.
- Disabling IPv6 (~6 KB) needs a custom ESP-IDF sdkconfig; it is baked into the
  precompiled Arduino libs. Not worth the maintenance at 25% flash use.

## Untested

No hardware verification is possible from the dev machine. Current draw, actual
battery life, and the battery-cutoff path have never been measured on device.
