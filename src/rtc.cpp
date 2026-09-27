#include "rtc.h"

#include <Wire.h>

#include "board.h"

namespace {

constexpr uint8_t REG_CONTROL2 = 0x01;
constexpr uint8_t REG_SECONDS = 0x02;

// Seconds register bit 7: oscillator stopped, time unreliable.
constexpr uint8_t FLAG_VL = 0x80;

constexpr uint8_t kDaysInMonth[12] = {31, 28, 31, 30, 31, 30,
                                      31, 31, 30, 31, 30, 31};

uint8_t bcdToBin(uint8_t v) { return (uint8_t)((v >> 4) * 10 + (v & 0x0F)); }
uint8_t binToBcd(uint8_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

bool isLeapYear(uint16_t y) {
  return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

// Latched from the seconds register on every time read, so checking it costs
// no bus transaction of its own. Starts set: an unread chip is not trusted.
bool g_integrity_lost = true;

bool rtcRead(RtcTime &out) {
  Wire.beginTransmission(RTC_I2C_ADDR);
  Wire.write(REG_SECONDS);
  if (Wire.endTransmission() != 0 ||
      Wire.requestFrom((int)RTC_I2C_ADDR, 7) != 7) {
    g_integrity_lost = true;
    return false;
  }

  uint8_t raw[7];
  for (uint8_t i = 0; i < 7; i++) {
    raw[i] = Wire.read();
  }

  g_integrity_lost = (raw[0] & FLAG_VL) != 0;
  out.second = bcdToBin(raw[0] & 0x7F);
  out.minute = bcdToBin(raw[1] & 0x7F);
  out.hour = bcdToBin(raw[2] & 0x3F);
  out.day = bcdToBin(raw[3] & 0x3F);
  out.weekday = (uint8_t)(raw[4] & 0x07);
  out.month = bcdToBin(raw[5] & 0x1F);
  // Century bit set means 1900s in this chip's convention.
  out.year = (uint16_t)(bcdToBin(raw[6]) + ((raw[5] & 0x80) ? 1900 : 2000));
  return true;
}

uint32_t rtcToEpoch(const RtcTime &t) {
  uint32_t days = 0;
  for (uint16_t y = 1970; y < t.year; y++) {
    days += isLeapYear(y) ? 366 : 365;
  }
  for (uint8_t m = 1; m < t.month; m++) {
    days += kDaysInMonth[m - 1];
    if (m == 2 && isLeapYear(t.year)) {
      days++;
    }
  }
  days += (uint32_t)(t.day - 1);
  return days * 86400UL + t.hour * 3600UL + t.minute * 60UL + t.second;
}

}  // namespace

void rtcBegin(bool cold_boot) {
  // The PCF8563 is rated for fast mode; at 400 kHz the per-wake time read
  // holds the bus a quarter as long.
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 400000);

  // No alarm is ever armed. Clear any left by older firmware so it cannot
  // hold the interrupt line asserted -- once per flash, not per wake.
  if (cold_boot) {
    Wire.beginTransmission(RTC_I2C_ADDR);
    Wire.write(REG_CONTROL2);
    Wire.write((uint8_t)0x00);
    Wire.endTransmission();
  }
}

void rtcEnd() { Wire.end(); }

bool rtcClockIntegrityLost() { return g_integrity_lost; }

bool rtcReadEpoch(uint32_t &epoch) {
  RtcTime t;
  if (!rtcRead(t)) {
    return false;
  }
  epoch = rtcToEpoch(t);
  return true;
}

bool rtcWriteEpoch(uint32_t epoch) {
  RtcTime t;
  rtcFromEpoch(epoch, t);

  Wire.beginTransmission(RTC_I2C_ADDR);
  Wire.write(REG_SECONDS);
  // Writing seconds with bit 7 clear also clears VL, marking the time valid.
  Wire.write(binToBcd(t.second));
  Wire.write(binToBcd(t.minute));
  Wire.write(binToBcd(t.hour));
  Wire.write(binToBcd(t.day));
  Wire.write(t.weekday);
  Wire.write(binToBcd(t.month));
  Wire.write(binToBcd((uint8_t)(t.year % 100)));
  return Wire.endTransmission() == 0;
}

void rtcFromEpoch(uint32_t epoch, RtcTime &out) {
  uint32_t days = epoch / 86400UL;
  uint32_t rem = epoch % 86400UL;

  out.hour = (uint8_t)(rem / 3600);
  out.minute = (uint8_t)((rem % 3600) / 60);
  out.second = (uint8_t)(rem % 60);
  out.weekday = (uint8_t)((days + 4) % 7);  // 1970-01-01 was a Thursday

  uint16_t year = 1970;
  for (;;) {
    uint16_t len = isLeapYear(year) ? 366 : 365;
    if (days < len) {
      break;
    }
    days -= len;
    year++;
  }
  out.year = year;

  uint8_t month = 1;
  for (;;) {
    uint16_t len = kDaysInMonth[month - 1];
    if (month == 2 && isLeapYear(year)) {
      len++;
    }
    if (days < len) {
      break;
    }
    days -= len;
    month++;
  }
  out.month = month;
  out.day = (uint8_t)(days + 1);
}
