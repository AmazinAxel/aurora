// Aurora — pomodoro e-ink watch firmware.
//
// Everything runs inside setup(): each wake does its work and deep sleeps
// again, so there is no loop. Deep sleep is the only idle state.

#include <Arduino.h>
#include <esp_sleep.h>

#include "../AuroraSettings.h"
#include "buttons.h"
#include "display.h"
#include "net.h"
#include "pomodoro.h"
#include "power.h"
#include "rtc.h"
#include "state.h"
#include "timekeeping.h"

namespace {

// A depleted cell rebounds once the load drops, so resuming needs a clear
// margin or the watch flaps in and out of shutdown.
constexpr uint16_t kBatteryRecoverMv = 3600;

// Aim this far past each deadline. The sleep timer runs off the ESP32's RC
// oscillator, not the RTC crystal, so a wake aimed exactly at a minute
// boundary can land a fraction early -- see the old minute, draw nothing, and
// need a second wake a second later. Arriving slightly late costs nothing.
constexpr uint32_t kWakeSlackMs = 700;

constexpr uint64_t kStuckButtonRecheckMs = 60000;

// millis() at the last RTC read, so the sleep can subtract the time the wake
// has spent since -- a long press or a slow refresh would otherwise push
// every deadline late by that much.
uint32_t g_now_read_ms = 0;

uint32_t readNow() {
  g_now_read_ms = millis();
  return timeNowUtc();
}

// Already on the cover means the panel already shows it: redrawing would be
// a full refresh for an identical image. The cover is "off", so a timer does
// not survive it: nothing buzzes there, and nothing resumes on the way back.
void enterCover() {
  if (g_state.screen == SCREEN_COVER) {
    return;
  }
  pomodoroStop();
  g_state.preset_shown_until = 0;
  g_state.screen = SCREEN_COVER;
  displayCover();
}

// sleep_ms 0 sleeps until a button, with no timer at all.
[[noreturn]] void deepSleep(uint64_t sleep_ms) {
  powerPrepareDeepSleep();

  // A stuck button is left out of the wake mask, so without this it could
  // never be armed again once freed.
  if (!buttonsArmWake() && sleep_ms == 0) {
    sleep_ms = kStuckButtonRecheckMs;
  }

  // The panel's BUSY wait may have armed light-sleep GPIO and timer wakes;
  // neither has any business in deep sleep.
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
  if (sleep_ms > 0) {
    esp_sleep_enable_timer_wakeup(sleep_ms * 1000ULL);
  } else {
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
  }

  // The ROM's boot banner is ~250 bytes of blocking 115200 baud UART per wake.
  esp_deep_sleep_disable_rom_logging();
  esp_deep_sleep_start();
}

void sleepNow(uint32_t delay_sec) {
  uint32_t awake_ms = millis() - g_now_read_ms;
  uint64_t target_ms = (uint64_t)delay_sec * 1000ULL + kWakeSlackMs;
  deepSleep(target_ms > awake_ms + 1000ULL ? target_ms - awake_ms : 1000ULL);
}

// Does not return if the watch is parked on a flat cell.
void handleBatteryShutdown() {
  uint16_t mv = powerBatteryMillivolts();

  if (g_state.battery_shutdown) {
    if (mv >= kBatteryRecoverMv) {
      // Stays on the cover until top left, which may be this very press.
      g_state.battery_shutdown = false;
      return;
    }
  } else if (mv > kBatteryCutoffMv) {
    return;
  } else {
    g_state.battery_shutdown = true;
    enterCover();
  }

  // No timer: a flat cell gets no wakes it did not ask for, and a press is
  // what re-checks it after charging.
  buttonsWaitForRelease(10000);
  deepSleep(0);
}

// True when top left leaves a cover that was charged on, which wants an NTP
// sync. After the draw, so it costs the press no latency.
bool handleButton(Button button, uint32_t now_utc) {
  // A spurious EXT1 wake with no pin latched must not count as a press --
  // least of all as the one that acknowledges an alert.
  if (button == BTN_NONE) {
    return false;
  }

  // On the cover the watch is "off": top left is the power button and the
  // only thing that brings it back, so the rest are swallowed rather than
  // acting on a watch the user thinks is asleep.
  if (g_state.screen == SCREEN_COVER) {
    if (button == BTN_TOP_LEFT) {
      g_state.screen = SCREEN_WATCHFACE;
      // The cover takes no samples, so this is the first look at any charge
      // since. Consuming it here also stops it reading as a plug-in on the
      // next tick, which would send the watch straight back.
      return powerChargerPlugged();
    }
    return false;
  }

  // While buzzing, any press acknowledges and starts the next phase. That is
  // the whole interaction, so it pre-empts the normal mapping.
  if (pomodoroAlerting()) {
    pomodoroAcknowledge(now_utc);
    return false;
  }

  // Top right has no hold action, so it acts on the press rather than waiting
  // out the hold window before the panel starts refreshing.
  bool held = button != BTN_TOP_RIGHT && buttonsWaitForHold(button);

  switch (button) {
    case BTN_TOP_LEFT:
      if (held) {
        enterCover();
        return false;
      }
      pomodoroStop();
      g_state.preset_shown_until = 0;
      break;

    case BTN_BOTTOM_LEFT:
      if (held) {
        pomodoroStart(now_utc);
      } else {
        pomodoroTogglePause(now_utc);
      }
      g_state.preset_shown_until = 0;
      break;

    case BTN_TOP_RIGHT:
      pomodoroCyclePreset(1);
      g_state.preset_shown_until = now_utc + PRESET_VISIBLE_SECONDS;
      break;

    case BTN_BOTTOM_RIGHT:
      if (held) {
        g_state.dst_active = !g_state.dst_active;
      } else {
        pomodoroCyclePreset(-1);
        g_state.preset_shown_until = now_utc + PRESET_VISIBLE_SECONDS;
      }
      break;

    default:
      break;
  }
  return false;
}

// Charging means usually near known WiFi, so it is the cheapest time to correct
// drift. The result is not displayed; a failure just means the next charge
// tries again. Returns the current time: the sync can take seconds, success or
// not.
uint32_t syncNtp() {
  uint32_t true_utc = 0;
  if (netSyncNtp(true_utc)) {
    timeApplyNtp(true_utc);
  }
  return readNow();
}

// Timer wakes only: a button wake is a user action, and the rail reads high
// there anyway from whatever the press just did. Returns the current time,
// which an NTP sync may have moved.
uint32_t handleCharger(uint32_t now_utc) {
  if (!powerChargerPlugged()) {
    return now_utc;
  }

  // Parked on the charger, so the panel holds one image for hours -- same
  // reasoning as the manual cover entry.
  enterCover();

  return syncNtp();
}

// The clock wants the next minute boundary; a running pomodoro wants its
// phase end; a shown preset wants its expiry. Whichever comes first wins.
uint32_t nextWakeDelay(uint32_t now_utc) {
  // Display offsets are whole minutes, so the local minute turns over with
  // the UTC one. A dead RTC reads as 0; a plain minute keeps it ticking.
  uint32_t next = now_utc + (now_utc == 0 ? 60 : 60 - now_utc % 60);

  for (uint32_t deadline :
       {pomodoroNextWakeUtc(now_utc), g_state.preset_shown_until}) {
    if (deadline > now_utc && deadline < next) {
      next = deadline;
    }
  }

  return next - now_utc;
}

}  // namespace

void setup() {
  // Already at 40 MHz: board_build.f_cpu has the core set it before setup().
  bool cold_boot = stateInitIfCold();

  buttonsBegin();
  rtcBegin(cold_boot);
  if (cold_boot) {
    powerIsolateUnusedPins();
    powerSuspendAccel();
  }

  // The only RTC read on an ordinary wake; everything below reuses it.
  uint32_t now_utc = readNow();

  // Before anything touches the panel: a brownout mid-update can leave the
  // e-ink partially driven.
  handleBatteryShutdown();

  bool sync_after_draw = false;
  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT1) {
    // Tick first, so a press just after a phase ended acknowledges it rather
    // than pausing a timer that has already run out.
    pomodoroTick(now_utc);
    sync_after_draw = handleButton(
        buttonsFromWakeMask(esp_sleep_get_ext1_wakeup_status()), now_utc);
  } else {
    now_utc = handleCharger(now_utc);
    pomodoroTick(now_utc);
  }

  // The cover is an explicit "put it away" state: silent and undrawn.
  if (g_state.screen != SCREEN_COVER) {
    // The pulse overlaps the panel refresh rather than preceding it, so a
    // buzzing wake is no longer than a drawing one.
    bool buzz = pomodoroShouldBuzz(now_utc);
    uint32_t buzz_start = millis();
    if (buzz) {
      powerVibrate(true);
    }

    // Unconditional: the content compare inside turns an unchanged frame into
    // a no-op, so there is no need to track here whether anything changed.
    displayWatchface(now_utc, cold_boot);

    if (buzz) {
      uint32_t elapsed = millis() - buzz_start;
      if (elapsed < ALERT_PULSE_ON_MS) {
        delay(ALERT_PULSE_ON_MS - elapsed);
      }
      powerVibrate(false);
    }
  }

  if (sync_after_draw) {
    now_utc = syncNtp();
  }

  // Bounded so a stuck button cannot hold the watch out of deep sleep.
  buttonsWaitForRelease(10000);

  // On the cover nothing is drawn and nothing buzzes, so only a button brings
  // it back.
  if (g_state.screen == SCREEN_COVER) {
    deepSleep(0);
  }

  sleepNow(nextWakeDelay(now_utc));
}

void loop() {}
