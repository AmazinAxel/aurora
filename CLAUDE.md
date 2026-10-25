# Aurora

Pomodoro e-ink watch firmware, USB-C Watchy-class ESP32 board (AliExpress clone, not SQFMI). Battery life + panel longevity above all.

## Hardware
Found via throwaway probe sketches (removed); this table is the only record.

| Part | Detail |
|---|---|
| MCU | ESP32 classic (not S3; matters for wake modes) |
| Panel | GDEH0154D67, 200x200 mono, SPI |
| RTC | PCF8563 @ I2C 0x51 |
| Accel | 0x18, unused |
| Buttons | active HIGH: TL=25, BL=26, TR=35, BR=4 |
| Motor | GPIO 13, output (as input = motor runs) |
| Battery | GPIO 34, 2:1 divider |

Traps:
- GPIO 33 = 32.768 kHz RTC crystal line, not button; oscillates ~30 Hz; in wake mask = constant wakes.
- GPIO 35 input-only, no internal pulldown; relies on external; `INPUT_PULLDOWN` silent no-op.
- ESP32 classic EXT1 only `ANY_HIGH`/`ALL_LOW` (no `ANY_LOW` until S3). Active-high buttons make `ANY_HIGH` work.
- Re-arm RTC pulldowns before sleep (`rtc_gpio_pulldown_en`); digital pulls drop, floating pins false-trigger `ANY_HIGH`.
- `adc_power_release()` aborts (boot loop); `analogReadMilliVolts` manages SAR refcount itself.

## Architecture
No `loop()`. All in `setup()`: wake, work, deep sleep. Deep sleep = only idle state.

```
wake (timer or EXT1 button)      boots at 80 MHz (board_build.f_cpu)
  -> stateInitIfCold()             RTC_DATA_ATTR struct, zeroed on cold boot
  -> rtcBegin() / readNow()        only RTC read; everything reuses it
  -> handleBatteryShutdown()       before panel touched
  -> button: pomodoroTick() then handleButton()
     timer:  handleCharger() then pomodoroTick()
  -> motor on if buzzing, displayWatchface(), motor off
                                   pulse overlaps refresh; draw skipped if hash matches
  -> wait for button release
  -> sleepNow(nextWakeDelay())     minus time awake, plus slack
```

| File | Role |
|---|---|
| `src/main.cpp` | wake dispatch, button map, sleep scheduling |
| `src/state.h/.cpp` | `RTC_DATA_ATTR` persisted struct |
| `src/rtc.cpp` | PCF8563 over raw `Wire`, no lib |
| `src/timekeeping.cpp` | drift model, TZ/DST/minutes-ahead |
| `src/display.cpp` | all drawing + refresh policy |
| `src/pomodoro.cpp` | phase state machine |
| `src/power.cpp` | CPU clock, battery ADC, motor |
| `src/net.cpp` | NTP, guaranteed radio teardown |
| `src/buttons.cpp` | debounce, hold |
| `src/board.h` | pins |
| `tools/` | host-side asset + font gen (build-time only) |
| `AuroraSettings.h` | user prefs (root) |

## Timekeeping
- RTC holds true UTC. TZ, DST, `MINUTES_AHEAD` applied only for display; timebase stays NTP-comparable.
- Drift self-calibrates per sync: chip elapsed vs true elapsed → ppm, EMA (`DRIFT_EMA_ALPHA`); rejected > `DRIFT_MAX_PPM` or interval < `DRIFT_MIN_INTERVAL_SEC` (NTP RTT noise).
- Wakes use ESP32 timer, not RTC alarm. Old version armed PCF8563 alarm but never enabled EXT0 → never fired. Alarm path removed; chip = timebase only.

## Display / ink policy
`displayWatchface` / `displayCover` = only entry points; policy can't be bypassed.
- Content → struct → FNV-1a hash vs `g_state.drawn_hash`. Unchanged → no panel drive, panel never init (`ensureInitialised` lazy). Biggest saving.
- Partial by default. Full on cold boot, on cover, every `FULL_REFRESH_INTERVAL` (180) partials (panel spec; ghosting = reversible trapped charge, not damage). Entering cover full (dwells hours); leaving partial.
- `hibernate()` before every sleep.

### Layout
Three columns: 23 px left strip, clock, 8 px pomodoro bar right. No pomodoro → clock band shifts right (`kIdleClockLeft`/`Right`). Width = binding constraint on 200 px panel.

```
┌────┬─────────────┬─┐
│ 0  │             │ │   strip, one glyph per row:
│ 9  │     14      │█│     month digits
│    │             │█│     day digits
│ 1  │             │█│     weekday, two letters (SU..SA)
│ 5  │             │█│     slot: play/pause if pomodoro active,
│    │     32      │█│       else rule + battery below
│ W  │             │█│       BATTERY_VISIBLE_BELOW, else empty
│ E  │             │█│
│    │             │█│   clock: always two rows, FONT_TIME
│ ▶  │             │█│
└────┴─────────────┴─┘
```

- Clock rows priority: preset for `PRESET_VISIBLE_SECONDS` after cycling (focus top, break below, two digits: `05`); `--`/`--` while RTC integrity lost (until NTP writes); else time.
- Bar: full height, bottom-anchored, `remaining / phase` seconds rounded up; drains from top; redrawn each minute. Idle/buzzing: no bar (buzzing shows pause).

Typography:
- Strip: exactly one font + pitch. Month, day, weekday, battery all `FONT_STRIP`, two glyphs per block, one per row, 21 px apart. Two font sizes total on face. Keep it.
- Weekday two letters (`SU`..`SA`); three stacked read badly, cost a row.
- Strip glyphs centre on ink box, same axis. `1` ink box = advance centre here; no special case.
- Vertical budget = 8 glyph rows + battery rule; sets size. `FONT_STRIP` = SemiBold 13pt, 1.1x horizontal stretch (`xscale` arg, `make_fonts.sh`), double-struck 1 px vertical: 19 px tall, 3 px strokes, `W` 20 px in 23 px strip. 14pt fits only with ~3 px total margin.
- Blocks spaced on ink, 8 px top/bottom margin. Glyphs in block 2 px apart; month↔day 6 px; weekday 9 px off; battery 4 + rule + 4 below. Play/pause icon (15x19) centred in space below weekday.

## Fonts
- Barlow Semi Condensed (OFL). Narrow: clock bounded by two-digit width, narrow buys height. Strip = same face stretched, carries weekday letters.
- `tools/make_fonts.sh [ttf-dir]` → committed `src/assets/`. fontconvert takes contiguous range; `tools/subset_font.mjs` strips undrawn glyphs. Clock: `-0123456789` (`-` for invalid-time). Strip: digits + letters of `SU`..`SA`.
- Any other char must be added in `make_fonts.sh` or renders blank. No general text font.
- Adafruit GFX has no alignment API, only `getTextBounds`; all alignment computed.

## Power
- 80 MHz whole wake via `board_build.f_cpu` (never spins to 240 first). 240 MHz only in `netSyncNtp` (radio dominates; finish sooner).
- Panel BUSY wait light-sleeps (GxEPD2 busy callback, GPIO low wake + 1 s timer backstop), like official Watchy. Not while motor runs (pin may not hold). Deep sleep disables GPIO wake again.
- `radioOff()` on every NTP exit incl failures. Gives up at once on `WL_NO_SSID_AVAIL` / `WL_CONNECT_FAILED`.
- One RTC read per wake (`readNow`), 400 kHz I2C; VL integrity flag latched from same read.
- One battery ADC read per wake, cached in `power.cpp`, shared by shutdown, charger detect, display. Displayed % refreshes every 15 min (noise → redraws).
- Buzz pulse concurrent with refresh (else alert wake ~400 ms longer).
- Sleep length minus awake time, aims 700 ms past deadline (`kWakeSlackMs`): sleep timer on RC osc; landing before RTC minute turns = empty wake.
- < 3400 mV: park on cover, 10 min timer wakes only, buttons not wake source. Recover at 3600 mV (hysteresis). Above ~3.0 V brownout so shutdown deliberate, not mid-write reset.

## Controls
| | Tap | Hold (500 ms) |
|---|---|---|
| Top-left | Stop + clear pomodoro | Sleep to cover |
| Bottom-left | Pause / start | Restart focus |
| Top-right | Next preset | — |
| Bottom-right | Previous preset | Toggle DST |

- NTP: no button; auto-sync on charger unplug (reliably near known WiFi). Result not shown.
- Cover: only top-left responds; others swallowed. Entering cover while on it draws nothing.
- Top-right has no hold → acts without waiting hold window.
- Buzzing: any press acks + starts next phase.
- Preset change stops running timer (remaining measured against old durations).

## Pomodoro
- Phase ends locked to start instant, not wall clock (focus at 14:32:47 ends 15:02:47; wake scheduled for that second). Bar not: recomputed from remaining on clock minute tick (else double wakes for 1-2 px).
- State: `phase` (idle / focus / break / alert-after-focus / alert-after-break) + `paused`; paused stores `paused_remaining`, resume rebuilds end.
- Chain: focus → buzz until ack → break → buzz → focus. Pulsed (`ALERT_REPEAT_SEC`, one wake per buzz), gives up after `ALERT_MAX_DURATION_SEC`; phase still waits for press.

## Build
- `pio run -t upload`.
- Cover: `node tools/gen_assets.mjs src/aurora.png src/assets/cover.h` (200x200), by hand; `cover.h` committed.
- ~798 KB flash (25%), ~50 KB RAM; WiFi/lwIP for NTP dominates.
- Already measured, don't revisit: only `-DCORE_DEBUG_LEVEL=0` pays (~18 KB); `-Os`, section GC, `-fno-rtti`, `-fno-exceptions` nothing. Hand-rolled int formatting vs `snprintf` saves no flash (`_vfprintf_r` pulled by IDF); kept for runtime. IPv6 off (~6 KB) needs custom sdkconfig; not worth it.

## Untested
- No hardware verification from dev machine. Current draw, battery life, cutoff path never measured on device.
- BUSY light sleep unverified on this board (proven on official Watchy).
- Deliberately not taken without device: panel SPI > GxEPD2 default 4 MHz, `rtc_gpio_hold_en` motor pin low in deep sleep, cached WiFi BSSID/channel.
