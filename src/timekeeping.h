#pragma once

#include <stdint.h>

#include "rtc.h"

// The RTC holds true UTC. Timezone, DST and the minutes-ahead preference are
// applied on the way to the display only, so the stored timebase stays
// comparable with NTP.

uint32_t timeNowUtc();

// The chip's own uncorrected reading, which is what the drift model measures.
uint32_t timeRawRtc();

uint32_t timeNowDisplay();
void timeNowDisplayParts(RtcTime &out);

// Set the RTC from a fresh NTP reading and, if there is a usable previous
// sync, update the learned drift from the error accumulated over the interval.
void timeApplyNtp(uint32_t true_utc);
