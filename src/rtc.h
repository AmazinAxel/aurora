#pragma once

#include <stdint.h>

// PCF8563 driver. Written against the registers directly rather than pulling
// in a general-purpose RTC library: this is the only clock chip on the board
// and the whole surface needed is a BCD time read and write.

struct RtcTime {
  uint16_t year;
  uint8_t month;    // 1-12
  uint8_t day;      // 1-31
  uint8_t weekday;  // 0 = Sunday
  uint8_t hour;
  uint8_t minute;
  uint8_t second;
};

bool rtcBegin();
void rtcEnd();

// True if the chip reports its oscillator stopped, meaning the stored time is
// not trustworthy and needs an NTP sync.
bool rtcClockIntegrityLost();

bool rtcReadEpoch(uint32_t &epoch);
bool rtcWriteEpoch(uint32_t epoch);

void rtcFromEpoch(uint32_t epoch, RtcTime &out);
