#include "display.h"

#include <GxEPD2_BW.h>

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

const char *const kWeekdays[7] = {"SUN", "MON", "TUE", "WED",
                                  "THU", "FRI", "SAT"};

bool g_initialised = false;

// Deferred so a wake that finds nothing changed never clocks SPI or pulses
// the panel's reset line. initial=false skips the startup full clear, which
// would otherwise flash on every wake.
void ensureInitialised() {
  if (g_initialised) {
    return;
  }
  display.init(0, false, 2, false);
  display.setRotation(0);
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

// Centred in the band left over after the side strip, not on the panel.
void drawCenteredIn(const char *text, int16_t left, int16_t right, int16_t y,
                    const GFXfont *font) {
  display.setFont(font);
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(text, 0, y, &x1, &y1, &w, &h);
  display.setCursor((int16_t)(left + (right - left - (int16_t)w) / 2 - x1), y);
  display.print(text);
}

// What occupies the bottom row, which decides how tall the clock can be.
enum class BottomRow : uint8_t {
  None,     // two-row clock, largest digits
  Timer,    // minutes remaining plus a play/pause mark
  Preset,   // "30/5" while cycling
  Message,  // NTP result
};

struct WatchfaceContent {
  char month[4];
  char day[4];
  char battery[8];  // empty unless the cell is low enough to be worth showing
  char hour[4];
  char minute[4];
  const char *weekday;

  BottomRow bottom;
  char bottom_text[16];
  bool icon_playing;
};

// Hand-rolled rather than snprintf: every value here is a small unsigned
// integer, and the C library formatter drags in its float-capable variants.
char *put2(char *p, uint8_t value) {
  *p++ = (char)('0' + (value / 10) % 10);
  *p++ = (char)('0' + value % 10);
  return p;
}

char *putN(char *p, uint16_t value) {
  if (value >= 100) {
    *p++ = (char)('0' + value / 100);
  }
  if (value >= 10) {
    *p++ = (char)('0' + (value / 10) % 10);
  }
  *p++ = (char)('0' + value % 10);
  return p;
}

char *appendText(char *p, const char *s) {
  while (*s) {
    *p++ = *s++;
  }
  return p;
}

void buildWatchfaceContent(uint32_t now_utc, WatchfaceContent &c) {
  RtcTime t;
  timeNowDisplayParts(t);

  *put2(c.month, t.month) = '\0';
  *put2(c.day, t.day) = '\0';
  *put2(c.hour, t.hour) = '\0';
  *put2(c.minute, t.minute) = '\0';

  uint8_t battery = powerBatteryPercent(now_utc);
  if (battery < BATTERY_VISIBLE_BELOW) {
    // No percent sign: the rule above the digits is what marks this as the
    // battery, and a '%' costs more width than the strip has.
    *putN(c.battery, battery) = '\0';
  } else {
    c.battery[0] = '\0';
  }

  c.weekday = kWeekdays[t.weekday % 7];
  c.icon_playing = false;
  c.bottom_text[0] = '\0';

  // A fresh NTP result outranks everything, then a recent preset change, then
  // an active pomodoro. Whatever wins shrinks the clock to make room.
  if (g_state.ntp_status != NTP_NONE && now_utc < g_state.ntp_status_until) {
    c.bottom = BottomRow::Message;
    *appendText(c.bottom_text,
                g_state.ntp_status == NTP_OK ? "NTP OK" : "NTP FAIL") = '\0';
  } else if (now_utc < g_state.preset_shown_until) {
    c.bottom = BottomRow::Preset;
    char *p = putN(c.bottom_text, pomodoroFocusMinutes());
    *p++ = '/';
    *putN(p, pomodoroBreakMinutes()) = '\0';
  } else if (pomodoroActive()) {
    c.bottom = BottomRow::Timer;
    c.icon_playing = pomodoroRunning();
    *putN(c.bottom_text, pomodoroRemainingMinutes(now_utc)) = '\0';
  } else {
    c.bottom = BottomRow::None;
  }
}

// FNV-1a. A collision would cost one skipped update that the next minute
// corrects, so the cheap hash is the right trade.
uint32_t hashContent(const WatchfaceContent &c) {
  uint32_t h = 2166136261u;
  auto mix = [&h](const char *s) {
    while (*s) {
      h = (h ^ (uint8_t)*s++) * 16777619u;
    }
    h = (h ^ 0xFFu) * 16777619u;  // separator, so "1"+"23" != "12"+"3"
  };

  mix(c.month);
  mix(c.day);
  mix(c.battery);
  mix(c.hour);
  mix(c.minute);
  mix(c.weekday);
  mix(c.bottom_text);

  // Layout mode and icon change the picture without changing any string.
  h = (h ^ (uint8_t)c.bottom) * 16777619u;
  return (h ^ (uint8_t)c.icon_playing) * 16777619u;
}

// Geometry from the real glyph metrics, not by eye.
//   58 pt digits: two digits 148 px wide, 78 px tall
//   42 pt digits: two digits 106 px wide, 57 px tall
//   13 pt UI:     16 px digits; 24 px wide when rotated
//
// Everything but the clock shares one strip down the left edge, leaving the
// rest for the digits.
constexpr int16_t kMargin = 3;
constexpr int16_t kLeftColumn = 28;
constexpr int16_t kStripCentre = 13;

constexpr int16_t kBigHour = 97;
constexpr int16_t kBigMinute = 197;

constexpr int16_t kSmallHour = 66;
constexpr int16_t kSmallMinute = 131;
constexpr int16_t kSmallBottom = 196;

// Strip runs top to bottom as date, weekday, then battery. The battery only
// appears below BATTERY_VISIBLE_BELOW, so the bottom is usually blank --
// which is why it gets the far end rather than the middle.
constexpr int16_t kDigitPitch = 19;
constexpr int16_t kStripTop = 16;
constexpr int16_t kDateGap = 10;
constexpr int16_t kWeekdayTop = 96;
constexpr int16_t kBatteryRule = 152;
constexpr int16_t kBatteryTop = 172;

// Stacked digits, one per row. Centred on each glyph's advance rather than
// its ink box: a '1' is 6 px of ink whose stem sits hard right, so centring
// the box pushes the stem -- what the eye tracks down a column -- off axis.
// Returns the baseline the next row would use.
int16_t drawDigitColumn(const char *text, int16_t top) {
  display.setFont(FONT_UI);
  const GFXfont *font = FONT_UI;

  int16_t y = top;
  for (const char *p = text; *p; p++) {
    if (*p < '0' || *p > '9') {
      continue;
    }
    const GFXglyph &glyph = font->glyph[(uint8_t)*p - font->first];
    display.setCursor((int16_t)(kStripCentre - glyph.xAdvance / 2), y);
    display.write(*p);
    y = (int16_t)(y + kDigitPitch);
  }
  return y;
}

// `left` is the leftmost column the text may occupy, not the baseline: under
// rotation the glyphs extend left of the baseline by their ascent, so passing
// a small x directly would push most of the text off the panel.
void drawRotated(const char *text, int16_t left, int16_t centre_y,
                 const GFXfont *font) {
  display.setFont(font);

  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);

  display.setRotation(3);
  display.setCursor((int16_t)(display.height() - centre_y - w / 2 - x1),
                    (int16_t)(left - y1));
  display.print(text);
  display.setRotation(0);
}

// Primitives rather than glyphs: an icon font for two shapes is not worth it.
void drawPlayIcon(int16_t x, int16_t y, int16_t size) {
  display.fillTriangle(x, (int16_t)(y - size / 2), x, (int16_t)(y + size / 2),
                       (int16_t)(x + size), y, GxEPD_BLACK);
}

void drawPauseIcon(int16_t x, int16_t y, int16_t size) {
  int16_t bar = size / 3;
  display.fillRect(x, (int16_t)(y - size / 2), bar, size, GxEPD_BLACK);
  display.fillRect((int16_t)(x + size - bar), (int16_t)(y - size / 2), bar,
                   size, GxEPD_BLACK);
}

void drawWatchfaceContent(const WatchfaceContent &c) {
  display.setTextColor(GxEPD_BLACK);

  int16_t y = drawDigitColumn(c.month, kStripTop);
  drawDigitColumn(c.day, (int16_t)(y + kDateGap));

  if (c.battery[0] != '\0') {
    constexpr int16_t kRuleWidth = 16;
    display.drawFastHLine((int16_t)(kStripCentre - kRuleWidth / 2),
                          kBatteryRule, kRuleWidth, GxEPD_BLACK);
    drawDigitColumn(c.battery, kBatteryTop);
  }

  // Drawn after every upright item: it flips the global rotation, and doing
  // that mid-sequence makes the following draws depend on it being restored.
  drawRotated(c.weekday, (int16_t)(kStripCentre - 7),
              (int16_t)(kWeekdayTop + 16), FONT_WEEKDAY);

  const int16_t clock_left = kLeftColumn;
  const int16_t clock_right = (int16_t)(display.width() - kMargin);

  if (c.bottom == BottomRow::None) {
    drawCenteredIn(c.hour, clock_left, clock_right, kBigHour, FONT_TIME_BIG);
    drawCenteredIn(c.minute, clock_left, clock_right, kBigMinute,
                   FONT_TIME_BIG);
    return;
  }

  drawCenteredIn(c.hour, clock_left, clock_right, kSmallHour, FONT_TIME_SMALL);
  drawCenteredIn(c.minute, clock_left, clock_right, kSmallMinute,
                 FONT_TIME_SMALL);

  if (c.bottom == BottomRow::Timer) {
    drawCenteredIn(c.bottom_text, clock_left, clock_right, kSmallBottom,
                   FONT_TIME_SMALL);

    // Positioned from the digits' own bounds so the icon tracks them rather
    // than sitting at a guessed offset.
    int16_t x1, y1;
    uint16_t w, h;
    display.setFont(FONT_TIME_SMALL);
    display.getTextBounds(c.bottom_text, 0, kSmallBottom, &x1, &y1, &w, &h);

    constexpr int16_t kIconSize = 20;
    int16_t text_left =
        (int16_t)(clock_left + (clock_right - clock_left - (int16_t)w) / 2);

    if (c.icon_playing) {
      drawPlayIcon((int16_t)(text_left - kIconSize - 10),
                   (int16_t)(y1 + (int16_t)h / 2), kIconSize);
    } else {
      drawPauseIcon((int16_t)(text_left - kIconSize - 10),
                    (int16_t)(y1 + (int16_t)h / 2), kIconSize);
    }
    return;
  }

  if (c.bottom == BottomRow::Preset) {
    drawCenteredIn(c.bottom_text, clock_left, clock_right,
                   (int16_t)(kSmallBottom - 4), FONT_PRESET);
    return;
  }

  // NTP messages are words, not digits, so they stay in the UI font.
  drawCenteredIn(c.bottom_text, clock_left, clock_right,
                 (int16_t)(kSmallBottom - 12), FONT_UI);
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
