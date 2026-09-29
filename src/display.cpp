#include "display.h"

#include <GxEPD2_BW.h>
#include <esp_sleep.h>

#include "../AuroraSettings.h"
#include "assets/cover.h"
#include "board.h"
#include "fonts.h"
#include "pomodoro.h"
#include "power.h"
#include "state.h"
#include "timekeeping.h"

namespace {

// Page height is the full panel so the frame is composed in one buffer: 5000
// bytes is affordable and it avoids the controller round-trips paging costs.
GxEPD2_BW<GxEPD2_154_D67, GxEPD2_154_D67::HEIGHT> display(
    GxEPD2_154_D67(PIN_EPD_CS, PIN_EPD_DC, PIN_EPD_RST, PIN_EPD_BUSY));

const char *const kWeekdays[7] = {"SU", "MO", "TU", "WE", "TH", "FR", "SA"};

bool g_initialised = false;

// A refresh is hundreds of milliseconds of the CPU polling BUSY. Light sleep
// until the line drops instead, as the official Watchy firmware does. The
// timer is a backstop so a panel that never releases BUSY still reaches
// GxEPD2's timeout. Skipped while the motor runs: its pin is not guaranteed
// to hold through light sleep, and the pulse overlaps the refresh.
void busyLightSleep(const void *) {
  if (powerVibrating()) {
    delay(1);
    return;
  }
  gpio_wakeup_enable((gpio_num_t)PIN_EPD_BUSY, GPIO_INTR_LOW_LEVEL);
  esp_sleep_enable_gpio_wakeup();
  esp_sleep_enable_timer_wakeup(1000000ULL);
  esp_light_sleep_start();
}

// Deferred so a wake that finds nothing changed never clocks SPI or pulses
// the panel's reset line. initial=false skips the startup full clear, which
// would otherwise flash on every wake.
void ensureInitialised() {
  if (g_initialised) {
    return;
  }
  display.init(0, false, 2, false);
  display.epd2.setBusyCallback(busyLightSleep);
  display.setTextColor(GxEPD_BLACK);
  g_initialised = true;
}

// Every draw goes through here, so the ghosting counter cannot drift out of
// step with what the panel actually did.
bool consumeRefreshMode(bool force_full) {
  bool full = force_full || g_state.partial_count >= FULL_REFRESH_INTERVAL;
  g_state.partial_count = full ? 0 : g_state.partial_count + 1;
  return full;
}

// The strip's bottom slot: the pomodoro mark while one is active, otherwise
// the battery when low, otherwise nothing.
enum Icon : uint8_t {
  ICON_NONE = 0,
  ICON_PLAY,
  ICON_PAUSE,
};

// Bytes only, no padding, so the whole struct hashes as-is.
struct WatchfaceContent {
  char month[3];
  char day[3];
  char battery[3];  // empty unless low enough to show, and never an icon too
  char hour[3];     // or the preset's focus minutes while it is shown
  char minute[3];   // or its break minutes
  uint8_t weekday;
  uint8_t icon;
  uint8_t bar;      // pomodoro bar height in px, 0 when none
};

// Hand-rolled rather than snprintf: every value here is a small unsigned
// integer. Always two digits, so a 5 minute break reads "05".
void put2(char *p, uint16_t value) {
  p[0] = (char)('0' + (value / 10) % 10);
  p[1] = (char)('0' + value % 10);
  p[2] = '\0';
}

void buildWatchfaceContent(uint32_t now_utc, WatchfaceContent &c) {
  memset(&c, 0, sizeof(c));

  RtcTime t;
  timeDisplayParts(now_utc, t);
  put2(c.month, t.month);
  put2(c.day, t.day);
  c.weekday = t.weekday % 7;

  // A recent preset change takes over the clock rows: focus on top, break
  // below, in the same digits the time uses.
  if (now_utc < g_state.preset_shown_until) {
    put2(c.hour, pomodoroFocusMinutes());
    put2(c.minute, pomodoroBreakMinutes());
  } else if (rtcClockIntegrityLost()) {
    // Plausible-looking nonsense would be worse than no time at all. Stays
    // until a sync writes the chip, which clears the flag.
    strcpy(c.hour, "--");
    strcpy(c.minute, "--");
  } else {
    put2(c.hour, t.hour);
    put2(c.minute, t.minute);
  }

  if (pomodoroActive()) {
    // An unacknowledged phase end is waiting on the user, so it reads as
    // paused. Its bar is gone: the phase has fully drained.
    c.icon = pomodoroPaused() || pomodoroAlerting() ? ICON_PAUSE : ICON_PLAY;

    uint32_t total = pomodoroPhaseSeconds();
    uint32_t left = pomodoroRemainingSeconds(now_utc);
    if (left > total) {
      left = total;  // an NTP sync can step the clock backwards mid-phase
    }
    if (total > 0) {
      // Rounded up, so a phase starts full and still shows a sliver in its
      // last seconds.
      c.bar = (uint8_t)((left * GxEPD2_154_D67::HEIGHT + total - 1) / total);
    }
  } else {
    uint8_t battery = powerBatteryPercent(now_utc);
    if (battery < BATTERY_VISIBLE_BELOW) {
      put2(c.battery, battery);
    }
  }
}

// FNV-1a. A collision would cost one skipped update that the next minute
// corrects, so the cheap hash is the right trade.
uint32_t hashContent(const WatchfaceContent &c) {
  uint32_t h = 2166136261u;
  const uint8_t *p = (const uint8_t *)&c;
  for (size_t i = 0; i < sizeof(c); i++) {
    h = (h ^ p[i]) * 16777619u;
  }
  return h;
}

// Layout, from the real glyph metrics rather than by eye:
//   70pt clock digits: 99 px tall, widest pair ("44") 155 px
//   13pt strip glyphs: 19 px tall double-struck, digits 12 px wide, "W" 20
//
// Three columns: the strip on the left, the clock, and the pomodoro bar on
// the right edge. With no pomodoro the bar's room is free, so the clock band
// shifts right to open the gap to the strip; it moves only when a timer
// starts or stops.
constexpr int16_t kStripCentre = 12;
constexpr int16_t kBarWidth = 8;
constexpr int16_t kBarLeft = GxEPD2_154_D67::WIDTH - kBarWidth;
constexpr int16_t kClockLeft = 23;
constexpr int16_t kClockRight = kBarLeft - 3;
constexpr int16_t kIdleClockLeft = 29;
constexpr int16_t kIdleClockRight = GxEPD2_154_D67::WIDTH - 1;

constexpr int16_t kHourBaseline = 97;
constexpr int16_t kMinuteBaseline = 197;

// The strip runs month, day, weekday, then the battery/icon slot, every
// block two glyphs on one pitch: 19 px of ink (17 above the baseline, 1
// below with the double strike) and 2 px between rows, so a block is 40 px.
// Month and day pair as the date; the weekday and battery stand further off:
//   8 + 40 + 6 + 40 + 9 + 40 + 4 + rule + 4 + 40 + 8
constexpr int16_t kStripPitch = 21;
constexpr int16_t kMonthBaseline = 25;    // ink 8..47
constexpr int16_t kDayBaseline = 71;      // ink 54..93
constexpr int16_t kWeekdayBaseline = 120; // ink 103..142

// The battery reads as one more block, the rule centred in the gap above
// it. The icon is not a glyph, so it centres in everything below the
// weekday (143..199) instead: 162..180.
constexpr int16_t kBatteryRule = 147;
constexpr int16_t kBatteryBaseline = 169; // ink 152..191
constexpr int16_t kIconCentre = 171;

// Stacked glyphs, one per row, each centred on its own ink box so every row
// shares the strip's axis exactly, whatever the glyph's side bearings.
//
// Drawn twice, one pixel apart vertically: SemiBold's 3 px stems stay 3 and
// its 2 px bars become 3, so strokes match both ways.
void drawStripColumn(const char *text, int16_t baseline) {
  const GFXfont *font = FONT_STRIP;
  display.setFont(font);
  for (const char *p = text; *p; p++) {
    const GFXglyph &glyph = font->glyph[(uint8_t)*p - font->first];
    int16_t x =
        (int16_t)(kStripCentre - (glyph.width - 1) / 2 - glyph.xOffset);
    display.setCursor(x, baseline);
    display.write(*p);
    display.setCursor(x, (int16_t)(baseline + 1));
    display.write(*p);
    baseline = (int16_t)(baseline + kStripPitch);
  }
}

// Centred on ink in the clock band, not on the panel.
void drawClockRow(const char *text, int16_t baseline, int16_t left,
                  int16_t right) {
  display.setFont(FONT_TIME);
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(text, 0, baseline, &x1, &y1, &w, &h);
  display.setCursor((int16_t)(left + (right - left - (int16_t)w) / 2 - x1),
                    baseline);
  display.print(text);
}

// Primitives rather than glyphs: an icon font for two shapes is not worth
// it. 15 x 19 px, the strip glyphs' height.
void drawIcon(uint8_t icon) {
  constexpr int16_t kLeft = kStripCentre - 7;
  constexpr int16_t kSize = 15;
  constexpr int16_t kTop = kIconCentre - 9;
  constexpr int16_t kHeight = 19;

  if (icon == ICON_PLAY) {
    display.fillTriangle(kLeft, kTop, kLeft, kTop + kHeight - 1,
                         kLeft + kSize - 1, kIconCentre, GxEPD_BLACK);
  } else {
    constexpr int16_t kBar = 5;
    display.fillRect(kLeft, kTop, kBar, kHeight, GxEPD_BLACK);
    display.fillRect(kLeft + kSize - kBar, kTop, kBar, kHeight, GxEPD_BLACK);
  }
}

void drawWatchfaceContent(const WatchfaceContent &c) {
  drawStripColumn(c.month, kMonthBaseline);
  drawStripColumn(c.day, kDayBaseline);
  drawStripColumn(kWeekdays[c.weekday], kWeekdayBaseline);

  if (c.icon != ICON_NONE) {
    drawIcon(c.icon);
  } else if (c.battery[0] != '\0') {
    // The rule is what marks these digits as the battery rather than a date.
    // Odd width, so it centres on the strip axis exactly.
    constexpr int16_t kRuleWidth = 15;
    display.drawFastHLine((int16_t)(kStripCentre - kRuleWidth / 2),
                          kBatteryRule, kRuleWidth, GxEPD_BLACK);
    drawStripColumn(c.battery, kBatteryBaseline);
  }

  bool idle = c.icon == ICON_NONE;
  int16_t left = idle ? kIdleClockLeft : kClockLeft;
  int16_t right = idle ? kIdleClockRight : kClockRight;
  drawClockRow(c.hour, kHourBaseline, left, right);
  drawClockRow(c.minute, kMinuteBaseline, left, right);

  // Anchored to the bottom edge, so it drains from the top down.
  if (c.bar > 0) {
    display.fillRect(kBarLeft, (int16_t)(display.height() - c.bar), kBarWidth,
                     c.bar, GxEPD_BLACK);
  }
}

}  // namespace

void displayWatchface(uint32_t now_utc, bool force_full) {
  WatchfaceContent content;
  buildWatchfaceContent(now_utc, content);

  // The cheapest refresh is the one that never happens: no current, no drive
  // cycle, and the ghosting counter does not advance either.
  uint32_t hash = hashContent(content);
  if (!force_full && hash == g_state.drawn_hash) {
    return;
  }

  ensureInitialised();

  // Window mode selects the waveform: a full window flashes and clears
  // ghosting, a partial window updates without flashing.
  if (consumeRefreshMode(force_full)) {
    display.setFullWindow();
  } else {
    display.setPartialWindow(0, 0, display.width(), display.height());
  }

  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    drawWatchfaceContent(content);
  } while (display.nextPage());

  g_state.drawn_hash = hash;
}

void displayCover() {
  // The one place a full refresh earns its cost: the cover is large solid
  // black and the watch then sits on it for hours, and dwelling on
  // un-neutralised charge is what makes ghosting hard to clear later.
  ensureInitialised();
  consumeRefreshMode(true);

  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.drawBitmap(0, 0, COVER_BITMAP, COVER_WIDTH, COVER_HEIGHT,
                       GxEPD_BLACK);
  } while (display.nextPage());

  // The panel no longer holds the watchface, so the next draw must not be
  // skipped as a no-op.
  g_state.drawn_hash = 0;
}

void displayHibernate() {
  if (g_initialised) {
    display.hibernate();
  }
}
