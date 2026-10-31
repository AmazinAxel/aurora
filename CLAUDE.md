# Aurora

Pomodoro e-ink watch firmware, USB-C Watchy-class ESP32 board (AliExpress clone, not SQFMI). Battery life + panel longevity above all.

## Hardware
Found via throwaway probe sketches (removed); this table is the only record.

| Part | Detail |
|---|---|
| MCU | ESP32 classic (not S3; matters for wake modes) |
| Panel | GDEH0154D67 class, 200x200 mono, SPI; driven with GxEPD2 `GxEPD2_154_GDEY0154D67` for fast full update (`0x1A`=100C, `0xD7`), every full refresh incl cover; image can go weak in cold |
| RTC | PCF8563 @ I2C 0x51 |
| Accel | 0x18, unused |
| Buttons | active HIGH: TL=25, BL=26, TR=35, BR=4 |
| Motor | GPIO 13, output (as input = motor runs); held low in deep sleep, `powerVibrate` releases |
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
wake (timer or EXT1 button)      40 MHz by setup() (board_build.f_cpu)
  -> stateInitIfCold()             RTC_DATA_ATTR struct, zeroed on cold boot
  -> rtcBegin() / readNow()        only RTC read; everything reuses it
  -> handleBatteryShutdown()       before panel touched
  -> button: pomodoroTick() then handleButton()
     timer:  handleCharger() then pomodoroTick()
  -> motor on if buzzing, displayWatchface(), motor off
                                   pulse overlaps refresh; draw skipped if content matches
  -> NTP sync if top-left just left a cover that was charged on
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
- Content → struct → compare vs `g_state.drawn_content`. Unchanged → no panel drive, panel never init (`initPanel` only on a draw; at most one draw per wake). Biggest saving. Changed → partial window narrowed to what differs (`setChangedWindow`), and what falls outside it is not drawn; relies on controller RAM surviving hibernate, and on `kClockRowSplit`/`kStripWidth` staying between the ink (recheck if fonts or baselines change).
- Partial by default. Full on cold boot, on cover, every `FULL_REFRESH_INTERVAL` (180) partials (no datasheet source; ghosting = reversible trapped charge, not damage). Entering cover full (dwells hours); leaving partial. No periodic refresh while on cover: nightly use stays under the 24 h guidance.
- `hibernate()` right after every draw, not at sleep: partial refresh leaves panel high voltage on, and release wait or NTP can follow.

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
- 40 MHz (XTAL, PLL off) from `initArduino` on via `board_build.f_cpu`; prebuilt IDF startup before it runs at 160 MHz (`CONFIG_ESP32_DEFAULT_CPU_FREQ_MHZ`, can't change without rebuilding IDF). 240 MHz only in `netSyncNtp` (radio dominates; finish sooner). 40 MHz untested on device.
- ROM boot banner suppressed before every deep sleep (`esp_deep_sleep_disable_rom_logging`); untested whether this ESP32 ROM honours it.
- PSRAM: core links no `psramInit`/`esp_spiram_*` (checked in elf); no per-wake probe to skip.
- Panel BUSY wait light-sleeps (GxEPD2 busy callback, GPIO low wake + 1 s timer backstop), like official Watchy. While motor runs, idles on BUSY falling edge instead (`powerIdleUntilLow`; pin may not hold in light sleep). No polling anywhere in `src/`; NTP waits on WiFi/SNTP events. Left: fixed spec delays and BUSY re-check inside GxEPD2, deadlines as failure bounds. Button hold/release waits same (`powerLightSleepUntilLow`), not polled; release debounce light-sleeps too (`powerLightSleepMs`); no debounce before hold check (boot outlasts bounce, untested).
- `nvs_flash_init` wrapped to no-op (`-Wl,--wrap`), real one only in `netSyncNtp`; nothing else may use NVS outside it. Deep sleep disables GPIO wake again, and timer wake if it sets none.
- `radioOff()` on every NTP exit incl failures. Gives up at once on `WL_NO_SSID_AVAIL` / `WL_CONNECT_FAILED`. Last channel cached in RTC to skip scan; failure clears it. No BSSID, no eFuse/secure boot by choice.
- One RTC read per wake (`readNow`), 400 kHz I2C; VL integrity flag latched from same read.
- One battery ADC read per wake, cached in `power.cpp`, shared by shutdown, charger detect, display. Displayed % refreshes every 15 min (noise → redraws).
- Buzz pulse concurrent with refresh (else alert wake ~400 ms longer).
- Sleep length minus awake time, aims 700 ms past deadline (`kWakeSlackMs`): sleep timer on RC osc; landing before RTC minute turns = empty wake.
- < 3400 mV: park on cover, no timer, buttons only. A press re-checks; recover at 3600 mV (hysteresis), stays on cover until top-left. Above ~3.0 V brownout so shutdown deliberate, not mid-write reset.

## Controls
| | Tap | Hold (500 ms) |
|---|---|---|
| Top-left | Stop + clear pomodoro | Sleep to cover |
| Bottom-left | Pause / start | Restart focus |
| Top-right | Next preset | — |
| Bottom-right | Previous preset | Toggle DST |

- Charger: plug-in edge (rail step, timer wakes only) → cover, then NTP sync. No unplug detection; top-left leaves cover. Result not shown. Charged while on cover: seen as a rise when top-left leaves it; syncs after the draw.
- NTP steps > 1 day rejected unless never synced, RTC integrity lost, or next sync asks the same step within 60 s (`timeApplyNtp`; NTP unauthenticated).
- Held buttons left out of EXT1 mask (else instant re-wake loop); if nothing else would wake it, 60 s timer re-arms them.
- Cover: entering clears the pomodoro (hold, plug-in, flat battery); never resumes. No timer wake, barring a stuck button. Only top-left responds; others swallowed. Entering cover while on it draws nothing.
- Top-right has no hold → acts without waiting hold window.
- Buzzing: any press acks + starts next phase.
- Preset change stops running timer (remaining measured against old durations).

## Pomodoro
- Phase ends locked to start instant, not wall clock (focus at 14:32:47 ends 15:02:47; wake scheduled for that second). Bar not: recomputed from remaining on clock minute tick (else double wakes for 1-2 px).
- State: `phase` (idle / focus / break / alert-after-focus / alert-after-break) + `paused`; paused stores `paused_remaining`, resume rebuilds end.
- Chain: focus → buzz until ack → break → buzz → focus. Pulsed (`ALERT_REPEAT_SEC`, one wake per buzz), gives up after `ALERT_MAX_DURATION_SEC`; phase still waits for press.

## Build
- `pio run -t upload`. `tools/remove_firmware_bin.py` deletes `firmware.bin` (plaintext WiFi password) after upload or build.
- Cover: `bun tools/gen_assets.mjs src/aurora.png src/assets/cover.h` (200x200), by hand; `cover.h` committed.
- `partitions.csv`: `nvs`, factory `app`, `coredump`; no OTA/filesystem. `coredump` stays: without it prebuilt IDF prints an error over UART every wake. PlatformIO still writes `boot_app0.bin` to unused 0xe000.
- ~803 KB flash (20%), ~51 KB RAM; WiFi/lwIP for NTP dominates.
- Already measured, don't revisit: only `-DCORE_DEBUG_LEVEL=0` pays (~18 KB); `-Os`, section GC, `-fno-rtti`, `-fno-exceptions` nothing. Hand-rolled int formatting vs `snprintf` saves no flash (`_vfprintf_r` pulled by IDF); kept for runtime. IPv6 off (~6 KB) needs custom sdkconfig; not worth it.

## Untested
- No hardware verification from dev machine. Current draw, battery life, cutoff path never measured on device.
- BUSY light sleep unverified on this board (proven on official Watchy). Button-wait light sleep unverified anywhere (GPIO 35 low-level wake, release bounce vs EXT1).
- Unverified without device: panel SPI 20 MHz, motor pin hold, GPIO 12/14/27/32/33 isolate, PCF8563 CLKOUT off, accel id guard (only BMA423 written), narrowed partial window.
- WiFi channel cache and channel-only `WiFi.begin` untested on device.
- RTC_PERIPH not forced off: only GPIO 35 known to have external pulldown. Prebuilt bootloader already has `CONFIG_BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP`.

## Open tasks
None applied. Gains are estimates; nothing here measured. Most need device + current meter.

First flash, check in order:
- Motor buzzes on alert (pin hold release).
- Panel clean at 20 MHz; if corrupt, drop SPI first.
- Minute digits update cleanly, no clipping or blanks (`kClockRowSplit`, `kStripWidth`); stale regions = controller RAM not surviving hibernate, revert to whole window.
- Cover not inverted (`invert` arg in `displayCover`).
- Hold still registers with no debounce before hold check.
- NTP works: channel cache, event waits, NVS wrap (PHY calibration).
- Measure sleep current first: clone regulator/USB chip may dominate, then only hardware helps.

Latency / battery:
- 80 MHz on button wakes: faster compose, small battery cost, PLL relock cost unknown.
- Direct SSD1681 driver + 1bpp blit replacing GxEPD2 + Adafruit GFX: BUSY waits replace ~40 ms fixed reset/init delays (10+2+10+10+10 ms), faster draw, maybe 10-20 KB flash. Would also remove BUSY re-check loop, the last polling-like code. High effort.
- Pure ESP-IDF or hybrid build with own sdkconfig: prebuilt starts at 160 MHz with PLL, `CONFIG_ESP32_RTC_CLK_CAL_CYCLES=1024` (~7 ms per wake); could lower both, watchdogs off. Guess 50-100 KB flash (coredump ~12 KB, BT ~5, IPv6 ~6, WPA3/crypto). Largest boot gain; no toggles left without it. PlatformIO `framework = arduino, espidf`.
- Custom partial LUT (`0x32`): ~460 ms refresh is most visible latency, shortest path to faster. Risks ghosting/panel; tuning needs device.
- RTC_PERIPH explicit off: only if all buttons proven external-pulldown, else false-wake storm. IDF 5 source auto-powers it off with EXT1; this core's IDF 4.4 not checked, so internal pulldowns may already do nothing.
- Static IP: skips DHCP; latency only (sync on charger), breaks on other networks.
- Hand-rolled SNTP over UDP: few KB flash. Flash barely matters now (no validation at wake).
- 1 ADC sample on button wakes: under 1 ms, noisier reading raises false cover-exit syncs. Advised skip.

Panel lifespan:
- ~1M cycles (Waveshare generic, not this panel; unconfirmed) vs ~350k partials/yr with nightly cover = ~2.9 yr, ~526k/yr always on face = ~1.9 yr; GDEH datasheet rated life 5 yr. Fewer ticks = only big lever; 2 min ticks rejected.
- Nightly charge on cover keeps it under 24 h refresh guidance; 24 h cover refresh deliberately not added.
- Mostly white cover: cheapest retention gain, needs art redraw. Full refresh leaving cover: rejected for now (latency). Slow waveform: reverted on request; no source quantifies it.
- `FULL_REFRESH_INTERVAL` 180 has no datasheet source; reseller note says every 5 operations. Check new panel's datasheet.
- Fast waveform (forced 100C LUT) can go weak in cold.

Security:
- Password plaintext in device flash, `firmware.elf`, `net.cpp.o`; `.bin` auto-deleted (delete-after-upload hook untested). No eFuse or secure boot, by choice. Dedicated IoT/guest SSID password is the user-side defense. BSSID pin rejected.
- NTP step check beaten by attacker answering two syncs in a row, or the first sync.

Known limits:
- Small voltage rebound on cover exit can trigger unneeded sync on battery.
- Firmware can't stop charging; clone's charge circuit unverified for days on charger.
