// Aurora — pomodoro e-ink watch firmware.
//
// Everything runs inside setup(): each wake does its work and deep sleeps
// again, so there is no loop. Deep sleep is the only idle state.

#include <Arduino.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>

#include "../AuroraSettings.h"
#include "board.h"
#include "buttons.h"
#include "display.h"
#include "net.h"
#include "pomodoro.h"
#include "power.h"
#include "rtc.h"
#include "state.h"
#include "timekeeping.h"

namespace {

// Above the ESP32's ~3.0 V brownout threshold, so the watch parks itself
// deliberately rather than being reset mid-update -- a reset during a display
// write can leave the panel partially driven, which damages e-ink rather than
// merely ghosting it.
constexpr uint16_t kBatteryCutoffMv = 3400;

// A depleted cell rebounds once the load drops, so resuming needs a clear
// margin or the watch flaps in and out of shutdown.
constexpr uint16_t kBatteryRecoverMv = 3600;
constexpr uint32_t kBatteryShutdownCheckMin = 10;

// Never trust a wake to be the one we asked for: a stuck button or a spurious
// EXT1 trigger would otherwise let the clock sit stale indefinitely.
constexpr uint32_t kMaxSleepSec = 3600;

// Aim this far past each deadline. The sleep timer runs off the ESP32's RC
// oscillator, not the RTC crystal, so a wake aimed exactly at a minute
// boundary can land a fraction early -- see the old minute, draw nothing, and
// need a second wake a second later. Arriving slightly late costs nothing.
constexpr uint32_t kWakeSlackMs = 700;

// millis() at the last RTC read, so the sleep can subtract the time the wake
// has spent since -- a long press or a slow refresh would otherwise push
// every deadline late by that much.
uint32_t g_now_read_ms = 0;

uint32_t readNow() {
  g_now_read_ms = millis();
  return timeNowUtc();
}

// Already on the cover means the panel already shows it: redrawing would be
// a full refresh for an identical image.
void enterCover() {
  if (g_state.screen == SCREEN_COVER) {
    return;
  }
  g_state.screen = SCREEN_COVER;
  displayCover();
}

void deepSleep(uint64_t sleep_ms, bool buttons_wake) {
  displayHibernate();
  powerVibrateOff();
  rtcEnd();

  // The panel's BUSY wait may have armed a light-sleep GPIO wake; it has no
  // business in deep sleep.
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
  esp_sleep_enable_timer_wakeup(sleep_ms * 1000ULL);

  if (buttons_wake) {
    esp_sleep_enable_ext1_wakeup(BUTTON_WAKE_MASK, ESP_EXT1_WAKEUP_ANY_HIGH);

    // Digital pulls are switched off in deep sleep, so the pins would float
    // and could spuriously trigger the ANY_HIGH wake. GPIO 35 is input-only
    // with no internal pulldown and relies on the board's external one.
    rtc_gpio_pulldown_en((gpio_num_t)PIN_BTN_TOP_LEFT);
    rtc_gpio_pulldown_en((gpio_num_t)PIN_BTN_BOTTOM_LEFT);
    rtc_gpio_pulldown_en((gpio_num_t)PIN_BTN_BOTTOM_RIGHT);
  }

  esp_deep_sleep_start();
}

void sleepNow(uint32_t delay_sec) {
  uint32_t awake_ms = millis() - g_now_read_ms;
  uint64_t target_ms = (uint64_t)delay_sec * 1000ULL + kWakeSlackMs;
  deepSleep(target_ms > awake_ms + 1000ULL ? target_ms - awake_ms : 1000ULL,
            true);
}

// True if the watch is parked on a flat cell, in which case it has already
// slept and the caller must do nothing further.
bool handleBatteryShutdown(uint32_t now_utc) {
  uint16_t mv = powerBatteryMillivolts();

  if (g_state.battery_shutdown) {
    if (mv >= kBatteryRecoverMv) {
      g_state.battery_shutdown = false;
      g_state.screen = SCREEN_WATCHFACE;
      // The panel held one image for a long time -- flash it clean.
      displayWatchface(now_utc, true);
      return false;
    }
  } else if (mv > kBatteryCutoffMv) {
    return false;
  } else {
    g_state.battery_shutdown = true;
    enterCover();
  }

  // Buttons are deliberately not a wake source here: with the cell this low,
  // letting the user repeatedly wake a watch that can do nothing useful would
  // finish it off. Only the timer, to notice a charger.
  deepSleep((uint64_t)kBatteryShutdownCheckMin * 60ULL * 1000ULL, false);
  return true;
}

void handleButton(Button button, uint32_t now_utc) {
  // A spurious EXT1 wake with no pin latched must not count as a press --
  // least of all as the one that acknowledges an alert.
  if (button == BTN_NONE) {
    return;
  }

  // On the cover the watch is "off": top left is the power button and the
  // only thing that brings it back, so the rest are swallowed rather than
  // acting on a watch the user thinks is asleep.
  if (g_state.screen == SCREEN_COVER) {
    if (button == BTN_TOP_LEFT) {
      g_state.screen = SCREEN_WATCHFACE;
    }
    return;
  }

  // While buzzing, any press acknowledges and starts the next phase. That is
  // the whole interaction, so it pre-empts the normal mapping.
  if (pomodoroAlerting()) {
    pomodoroAcknowledge(now_utc);
    return;
  }

  // Top right has no hold action, so it acts on the press rather than waiting
  // out the hold window before the panel starts refreshing.
  bool held = button != BTN_TOP_RIGHT && buttonsWaitForHold(button);

  switch (button) {
    case BTN_TOP_LEFT:
      if (held) {
        enterCover();
        return;
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
}

// Timer wakes only: a button wake is a user action, and the rail reads high
// there anyway from whatever the press just did. Returns the current time,
// which an NTP sync may have moved.
uint32_t handleCharger(uint32_t now_utc) {
  switch (powerCheckCharger()) {
    case CHARGER_PLUGGED:
      // Parked on the charger, so the panel holds one image for hours --
      // same reasoning as the manual cover entry.
      enterCover();
      break;

    case CHARGER_UNPLUGGED: {
      // Coming off the charger is the one moment the watch is reliably in
      // hand and near known WiFi, so it is the cheapest time to correct
      // drift. The result is not displayed; a failure just means the next
      // unplug tries again.
      uint32_t true_utc = 0;
      if (netSyncNtp(true_utc)) {
        timeApplyNtp(true_utc);
      }
      g_state.screen = SCREEN_WATCHFACE;
      return readNow();  // the sync can take seconds, success or not
    }

    case CHARGER_NO_CHANGE:
      break;
  }
  return now_utc;
}

// The clock wants the next minute boundary; a running pomodoro wants its
// phase end; a shown preset wants its expiry. Whichever comes first wins.
uint32_t nextWakeDelay(uint32_t now_utc) {
  // On the cover nothing is drawn and nothing buzzes, so only a button should
  // bring it back. The timer still resumes correctly because the phase end is
  // an absolute instant.
  if (g_state.screen == SCREEN_COVER) {
    // Unless the charger put it there: unplug detection is edge-triggered on
    // consecutive samples, so sleeping an hour would mean noticing the unplug
    // an hour late. These wakes are charger-powered, so they are free.
    return g_state.charger_present ? 60 : kMaxSleepSec;
  }

  // Display offsets are whole minutes, so the local minute turns over with
  // the UTC one. A dead RTC reads as 0; a plain minute keeps it ticking.
  uint32_t next = now_utc + (now_utc == 0 ? 60 : 60 - now_utc % 60);

  for (uint32_t deadline :
       {pomodoroNextWakeUtc(now_utc), g_state.preset_shown_until}) {
    if (deadline > now_utc && deadline < next) {
      next = deadline;
    }
  }

  uint32_t delay_sec = next - now_utc;
  return delay_sec > kMaxSleepSec ? kMaxSleepSec : delay_sec;
}

}  // namespace

void setup() {
  // Already at 80 MHz: board_build.f_cpu has the core set it before setup().
  bool cold_boot = stateInitIfCold();

  buttonsBegin();
  rtcBegin(cold_boot);

  // The only RTC read on an ordinary wake; everything below reuses it.
  uint32_t now_utc = readNow();

  // Before anything touches the panel: a brownout mid-update can leave the
  // e-ink partially driven.
  if (handleBatteryShutdown(now_utc)) {
    return;
  }

  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT1) {
    // Tick first, so a press just after a phase ended acknowledges it rather
    // than pausing a timer that has already run out.
    pomodoroTick(now_utc);
    handleButton(buttonsFromWakeMask(esp_sleep_get_ext1_wakeup_status()),
                 now_utc);
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
      powerVibrateOn();
    }

    // Unconditional: the content hash inside turns an unchanged frame into a
    // no-op, so there is no need to track here whether anything changed.
    displayWatchface(now_utc, cold_boot);

    if (buzz) {
      uint32_t elapsed = millis() - buzz_start;
      if (elapsed < ALERT_PULSE_ON_MS) {
        delay(ALERT_PULSE_ON_MS - elapsed);
      }
      powerVibrateOff();
    }
  }

  // Wait for release, or sleeping would immediately re-trigger the ANY_HIGH
  // wake. Bounded so a stuck button cannot pin the CPU awake.
  uint32_t start = millis();
  while (buttonsAnyDown() && (millis() - start) < 10000) {
    delay(20);
  }

  sleepNow(nextWakeDelay(now_utc));
}

void loop() {}
