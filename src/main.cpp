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

bool g_needs_draw = false;
bool g_needs_full = false;

void requestDraw(bool full) {
  g_needs_draw = true;
  g_needs_full = g_needs_full || full;
}

// Partial even across a screen change: the panel handles a full-frame partial
// fine, and FULL_REFRESH_INTERVAL is what bounds ghosting, so flashing on
// every transition buys nothing and is the most power-hungry thing it does.
void switchScreen(uint8_t screen) {
  g_state.screen = screen;
  requestDraw(false);
}

void doNtpSync(uint32_t now_utc) {
  uint32_t true_utc = 0;
  if (netSyncNtp(true_utc)) {
    timeApplyNtp(true_utc);
    g_state.ntp_status = NTP_OK;
    now_utc = true_utc;
  } else {
    g_state.ntp_status = NTP_FAILED;
  }

  g_state.ntp_status_until =
      now_utc + (uint32_t)NTP_STATUS_VISIBLE_MINUTES * 60;
  switchScreen(SCREEN_WATCHFACE);
}

void enterCover() {
  g_state.screen = SCREEN_COVER;
  displayCover();
  g_needs_draw = false;
}

void sleepNow(uint32_t delay_sec) {
  esp_sleep_enable_timer_wakeup((uint64_t)delay_sec * 1000000ULL);
  esp_sleep_enable_ext1_wakeup(BUTTON_WAKE_MASK, ESP_EXT1_WAKEUP_ANY_HIGH);

  // Digital pulls are switched off in deep sleep, so the pins would float and
  // could spuriously trigger the ANY_HIGH wake. GPIO 35 is input-only with no
  // internal pulldown and relies on the board's external one.
  rtc_gpio_pulldown_en((gpio_num_t)PIN_BTN_TOP_LEFT);
  rtc_gpio_pulldown_en((gpio_num_t)PIN_BTN_BOTTOM_LEFT);
  rtc_gpio_pulldown_en((gpio_num_t)PIN_BTN_BOTTOM_RIGHT);

  displayHibernate();
  powerVibrateOff();
  rtcEnd();

  esp_deep_sleep_start();
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
    g_state.screen = SCREEN_COVER;
    displayCover();
  }

  // Buttons are deliberately not a wake source here: with the cell this low,
  // letting the user repeatedly wake a watch that can do nothing useful would
  // finish it off. Only the timer, to notice a charger.
  displayHibernate();
  powerVibrateOff();
  rtcEnd();
  esp_sleep_enable_timer_wakeup((uint64_t)kBatteryShutdownCheckMin * 60ULL *
                                1000000ULL);
  esp_deep_sleep_start();
  return true;
}

void handleButton(Button button, uint32_t now_utc) {
  bool held = buttonsWaitForHold(button);

  // On the cover the watch is "off": top left is the power button and the
  // only thing that brings it back, so the rest are swallowed rather than
  // acting on a watch the user thinks is asleep.
  if (g_state.screen == SCREEN_COVER) {
    if (button == BTN_TOP_LEFT) {
      switchScreen(SCREEN_WATCHFACE);
    }
    return;
  }

  // While buzzing, any press acknowledges and starts the next phase. That is
  // the whole interaction, so it pre-empts the normal mapping.
  if (pomodoroAlerting()) {
    powerVibrateOff();
    pomodoroAcknowledge(now_utc);
    switchScreen(SCREEN_WATCHFACE);
    return;
  }

  switch (button) {
    case BTN_BOTTOM_LEFT:
      if (held) {
        pomodoroReset(now_utc);
      } else {
        pomodoroTogglePause(now_utc);
      }
      g_state.preset_shown_until = 0;
      break;

    case BTN_TOP_LEFT:
      if (held) {
        enterCover();
        return;
      }
      pomodoroStop();
      g_state.preset_shown_until = 0;
      break;

    case BTN_TOP_RIGHT:
      if (held) {
        doNtpSync(now_utc);
        return;
      }
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
      return;
  }

  switchScreen(SCREEN_WATCHFACE);
}

// The clock wants the next wall-clock minute; a running pomodoro wants its own
// tick, deliberately offset from it; the preset and NTP rows expire on their
// own deadlines. Whichever comes first wins.
uint32_t nextWakeDelay(uint32_t now_utc) {
  // On the cover nothing is drawn and nothing buzzes, so only a button should
  // bring it back. The timer still resumes correctly because the phase end is
  // an absolute instant.
  if (g_state.screen == SCREEN_COVER) {
    return kMaxSleepSec;
  }

  // A dead RTC reads as 0, which would land every wake on the same boundary.
  // Fall back to a plain minute so the watch keeps ticking regardless.
  uint32_t display_now = timeNowDisplay();
  uint32_t next =
      now_utc + (display_now == 0 ? 60 : 60 - (display_now % 60));

  uint32_t pomo = pomodoroNextWakeUtc(now_utc);
  if (pomo > now_utc && pomo < next) {
    next = pomo;
  }

  // Both rows shrink the clock while visible, so each needs a wake at its
  // deadline or the layout would not revert until the next minute tick.
  for (uint32_t deadline :
       {g_state.preset_shown_until, g_state.ntp_status_until}) {
    if (deadline > now_utc && deadline != UINT32_MAX && deadline < next) {
      next = deadline;
    }
  }

  uint32_t delay_sec = next > now_utc ? next - now_utc : 1;
  return delay_sec > kMaxSleepSec ? kMaxSleepSec : delay_sec;
}

}  // namespace

void setup() {
  // Most of a wake is spent waiting on the panel's BUSY line, so running that
  // at 240 MHz burns current for nothing.
  powerSetLowClock();

  bool cold_boot = stateInitIfCold();

  buttonsBegin();
  bool rtc_ok = rtcBegin();

  // Without a valid RTC the clock would be plausible-looking nonsense, so say
  // so instead and leave the message up until a sync succeeds.
  if (!rtc_ok || rtcClockIntegrityLost()) {
    g_state.ntp_status = NTP_FAILED;
    g_state.ntp_status_until = UINT32_MAX;
  }

  uint32_t now_utc = timeNowUtc();

  // Before anything touches the panel: a brownout mid-update can leave the
  // e-ink partially driven.
  if (handleBatteryShutdown(now_utc)) {
    return;
  }

  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT1) {
    handleButton(buttonsFromWakeMask(esp_sleep_get_ext1_wakeup_status()),
                 now_utc);
    now_utc = timeNowUtc();  // an NTP sync may have moved the timebase
  } else {
    pomodoroTick(now_utc);
    if (g_state.screen != SCREEN_COVER) {
      requestDraw(cold_boot);
    }
  }

  // The cover is an explicit "put it away" state, so it stays silent.
  if (g_state.screen != SCREEN_COVER && pomodoroShouldBuzz(now_utc)) {
    powerVibratePulse();
  }

  if (g_needs_draw && g_state.screen != SCREEN_COVER) {
    displayWatchface(now_utc, g_needs_full || cold_boot);
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
