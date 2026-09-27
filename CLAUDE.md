# Aurora

Pomodoro e-ink watch firmware for a USB-C Watchy-class ESP32 board
(AliExpress clone, not official SQFMI). Built for battery life and panel
longevity above all else.

## Hardware

Identified with throwaway probe sketches (I2C scan, buttons, VBUS), since
removed. The findings below are the record; nothing re-derives them.

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
  -> rtcBegin() / readNow()        the only RTC read; everything reuses it
  -> handleBatteryShutdown()       before anything touches the panel
  -> button: pomodoroTick() then handleButton()
     timer:  handleCharger() then pomodoroTick()
  -> motor on if buzzing, displayWatchface(), motor off
                                   the pulse overlaps the refresh; the draw
                                   is skipped entirely if the hash matches
  -> wait for button release
  -> sleepNow(nextWakeDelay())     minus time already awake, plus slack
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

Three columns: a 23 px strip down the left edge, the clock, and an 8 px
pomodoro bar on the right edge. With no pomodoro active the clock band shifts
right into the bar's free room (`kIdleClockLeft`/`Right`), widening the gap to
the strip.
**Width is the binding constraint on a 200 px square panel.**

```
┌────┬─────────────┬─┐
│ 0  │             │ │   strip, top to bottom:
│ 9  │     14      │█│     month digits (stacked, one per row)
│    │             │█│     day digits
│ 1  │             │█│     weekday (rotated, FONT_WEEKDAY 11pt)
│ 5  │     32      │█│     slot: play/pause while a pomodoro is
│ W  │             │█│       active, else rule + battery below
│ E  │             │█│       BATTERY_VISIBLE_BELOW, else empty
│ D  │             │█│
│    │             │█│   clock: always two rows, always FONT_TIME
│ ▶  │             │█│
└────┴─────────────┴─┘
```

The clock rows show, in priority order: the preset for
`PRESET_VISIBLE_SECONDS` after cycling (focus on top, break below, always two
digits so 5 reads `05`), `--`/`--` while the RTC reports lost integrity (until
an NTP sync writes it), otherwise the time.

The bar spans the full panel height, is anchored to the bottom edge, and is
`remaining / phase` of it in seconds, rounded up — full at start, draining from
the top, redrawn each minute. Idle or buzzing, there is no bar (a buzzing
phase shows pause).

Typography gotchas, all learned the hard way:

- Digit columns centre on **advance width, not ink box**. A `1` is narrow ink
  with its stem hard right; centring the box pushes the stem — what the eye
  tracks down a column — off axis.
- `drawRotated` takes the **leftmost column, not the baseline**. Under rotation
  glyphs extend left of the baseline by their ascent; passing a small x
  directly clips the text off-panel.
- The rotated weekday draws **last**, after every upright item, because it
  flips global rotation.
- Rotated text's **cap height becomes its width** — hence a separate smaller
  `FONT_WEEKDAY`, and strip digits sized so their ink width (15 px) matches it.
- Matching that width unscaled needs 27 px digits, which the strip's vertical
  budget cannot hold. So `FONT_STRIP` is Regular 14pt stretched 1.4x
  horizontally at generation time (the `xscale` arg in `make_fonts.sh`), and
  double-struck one pixel vertically like the weekday: 15 px wide, 20 px tall,
  3 px strokes both ways — the weekday's exact stroke. Stretching widens
  stems, so a heavier cut (SemiBold, Black) reads bolder than the weekday.
  Squashing further (12pt at 1.65x) was tried and hurt legibility.
- Strip blocks are spaced on **ink**, 8 px apart, with the weekday slot sized
  for "WED" (38 px). The battery and play/pause icon both centre in the space
  left below the weekday.

## Fonts

Barlow Semi Condensed (SIL OFL). Condensed on purpose: the clock is bounded
by two-digit width, so a narrow face buys height. The strip digits are the
same face, stretched (see above).

Generated by `tools/make_fonts.sh [ttf-dir]`, committed to `src/assets/`. Each
font carries only what it draws: the clock is `-` through `9` (the `-` is for
the invalid-time face), the strip is digits, the weekday is `A`–`Z`. **Drawing
any other character means widening its range in `make_fonts.sh`, or it renders
blank.** There is no general-purpose text font.

Adafruit GFX has **no text alignment API** — only `getTextBounds`. Every
alignment is computed.

## Power

- 80 MHz for the whole wake path; 240 MHz only inside `netSyncNtp`, because the
  radio dwarfs the core and finishing sooner wins.
- `radioOff()` runs on every NTP exit path including failures.
- One RTC read per wake (`readNow`), at 400 kHz I2C. The integrity (VL) flag is
  latched from that same read rather than fetched separately.
- One battery ADC read per wake, cached in `power.cpp` and shared by the
  shutdown check, charger detection and the display. The *displayed* percent
  only refreshes every 15 min (the reading is noisy; flicker would cost
  redraws).
- The buzz pulse runs concurrently with the panel refresh instead of before
  it, so an alert wake is not ~400 ms longer than an ordinary one.
- Sleep length subtracts the time already spent awake and aims 700 ms past the
  deadline (`kWakeSlackMs`): the sleep timer runs off the RC oscillator, and a
  wake landing just before the RTC's minute turns over would draw nothing and
  need a second wake.
- Below 3400 mV the watch parks on the cover and wakes on a 10 min timer only —
  buttons are not a wake source there. Recovers at 3600 mV (hysteresis stops
  flapping). Cutoff sits above the ~3.0 V brownout so it shuts down
  deliberately rather than being reset mid-panel-write.

## Controls

| | Tap | Hold (500 ms) |
|---|---|---|
| Top-left | Stop + clear pomodoro | Sleep to cover |
| Bottom-left | Pause / start | Restart focus |
| Top-right | Next preset | — |
| Bottom-right | Previous preset | Toggle DST |

NTP has no button: it syncs automatically when the charger is unplugged,
the one moment the watch is reliably near known WiFi. Success or failure is
not displayed.

On the cover, only top-left responds (power button); the rest are swallowed.
While buzzing, **any** press acknowledges and starts the next phase.

Changing preset stops a running timer — its remaining time was measured
against the old durations.

## Pomodoro

Phase ends are locked to the **start instant**, not the wall clock: a 30 min
focus begun at 14:32:47 ends exactly at 15:02:47, and a wake is scheduled for
that second. The bar is *not* — it is recomputed from remaining seconds on the
clock's minute tick. Stepping it on the phase's own second-offset would double
the wakes and refreshes for a difference of a pixel or two.

State is `phase` (idle / focus / break / alert-after-focus / alert-after-break)
plus a `paused` flag; a paused phase keeps its value and stores the span left
in `paused_remaining`, so resuming rebuilds the end instant.

Phases chain continuously: focus → buzz until acknowledged → break → buzz →
focus. Buzzing is pulsed (`ALERT_REPEAT_SEC`, one wake per buzz — a wake costs
far more than the pulse) and gives up after `ALERT_MAX_DURATION_SEC`, though
the phase still waits for a press.

## Build

```
pio run -t upload
```

The sleep image is generated from `src/aurora.png` (200x200) by
`node tools/gen_assets.mjs src/aurora.png src/assets/cover.h`, run by hand.
The generated `src/assets/cover.h` is committed, so the build needs neither.

~796 KB flash (25%), ~50 KB RAM. Dominated by the WiFi/lwIP stack that NTP
requires; the app itself is small.

Not worth revisiting, already measured:

- `-DCORE_DEBUG_LEVEL=0` is the only build flag that pays (~18 KB of core log
  strings). `-Os`, section GC, `-fno-rtti` and `-fno-exceptions` each change
  nothing — the core already applies them or makes them moot.
- Replacing `snprintf` with hand-rolled integer formatting saved **no flash** —
  `_vfprintf_r` is pulled in by ESP-IDF regardless. Kept only because it avoids
  runtime formatting work.
- Disabling IPv6 (~6 KB) needs a custom ESP-IDF sdkconfig; it is baked into the
  precompiled Arduino libs. Not worth the maintenance at 25% flash use.

## Untested

No hardware verification is possible from the dev machine. Current draw, actual
battery life, and the battery-cutoff path have never been measured on device.

Candidates deliberately not taken without a device to test on: panel SPI above
GxEPD2's default 4 MHz, a CPU clock below 80 MHz during the BUSY wait, and
caching the WiFi BSSID/channel for faster NTP connects.
