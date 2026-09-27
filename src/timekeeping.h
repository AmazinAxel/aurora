#pragma once

#include <stdint.h>

#include "rtc.h"

// The RTC holds true UTC. Timezone, DST and the minutes-ahead preference are
// applied on the way to the display only, so the stored timebase stays
// comparable with NTP.

// Reads the chip, so call once per wake and pass the result around. 0 if the
// chip could not be read.
uint32_t timeNowUtc();

// Local calendar fields for a UTC instant. Every display offset is a whole
// number of minutes, so local minute boundaries fall on UTC ones.
void timeDisplayParts(uint32_t utc, RtcTime &out);

// Set the RTC from a fresh NTP reading and, if there is a usable previous
// sync, update the learned drift from the error accumulated over the interval.
void timeApplyNtp(uint32_t true_utc);
