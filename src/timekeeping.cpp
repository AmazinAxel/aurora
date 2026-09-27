#include "timekeeping.h"

#include <math.h>

#include "../AuroraSettings.h"
#include "state.h"

namespace {

int32_t driftCorrection(uint32_t raw_rtc) {
  if (!g_state.drift_valid || g_state.last_ntp_rtc == 0 ||
      raw_rtc <= g_state.last_ntp_rtc) {
    return 0;
  }

  // drift_ppm is microseconds gained per second, so the accumulated error is
  // elapsed * ppm / 1e6. Negated: a fast crystal needs time subtracted.
  uint32_t elapsed = raw_rtc - g_state.last_ntp_rtc;
  return (int32_t)lroundf(-(g_state.drift_ppm * (float)elapsed) / 1000000.0f);
}

int32_t displayOffsetSeconds() {
  int32_t offset = (int32_t)TZ_OFFSET_MINUTES * 60;
  if (g_state.dst_active) {
    offset += (int32_t)DST_OFFSET_MINUTES * 60;
  }
  return offset + (int32_t)MINUTES_AHEAD * 60;
}

// The chip's own uncorrected reading, which is what the drift model measures.
uint32_t timeRawRtc() {
  uint32_t raw = 0;
  return rtcReadEpoch(raw) ? raw : 0;
}

}  // namespace

uint32_t timeNowUtc() {
  uint32_t raw = timeRawRtc();
  if (raw == 0) {
    return 0;
  }

  int32_t base = g_state.last_ntp_utc != 0
                     ? (int32_t)(g_state.last_ntp_utc - g_state.last_ntp_rtc)
                     : 0;
  return (uint32_t)((int32_t)raw + base + driftCorrection(raw));
}

void timeDisplayParts(uint32_t utc, RtcTime &out) {
  rtcFromEpoch(utc == 0 ? 0 : (uint32_t)((int32_t)utc + displayOffsetSeconds()),
               out);
}

void timeApplyNtp(uint32_t true_utc) {
  // Measured before the chip is touched: this reading is what the crystal
  // drifted to on its own, and it is the only evidence of the rate.
  uint32_t raw_before = timeRawRtc();

  if (raw_before != 0 && g_state.last_ntp_rtc != 0 &&
      g_state.last_ntp_utc != 0 && raw_before > g_state.last_ntp_rtc &&
      true_utc > g_state.last_ntp_utc) {
    uint32_t rtc_elapsed = raw_before - g_state.last_ntp_rtc;
    uint32_t true_elapsed = true_utc - g_state.last_ntp_utc;

    // Over a short span the NTP round-trip error swamps the drift signal.
    if (rtc_elapsed >= DRIFT_MIN_INTERVAL_SEC) {
      float error = (float)((int32_t)rtc_elapsed - (int32_t)true_elapsed);
      float ppm = (error * 1000000.0f) / (float)true_elapsed;

      // Beyond this the cause is a reset or a bad sync, not a crystal.
      if (fabsf(ppm) <= DRIFT_MAX_PPM) {
        g_state.drift_ppm =
            g_state.drift_valid
                ? g_state.drift_ppm * (1.0f - DRIFT_EMA_ALPHA) +
                      ppm * DRIFT_EMA_ALPHA
                : ppm;
        g_state.drift_valid = true;
      }
    }
  }

  rtcWriteEpoch(true_utc);

  // Re-read rather than assuming the write landed exactly: it truncates to
  // whole seconds, and the baseline must match what the chip will report.
  uint32_t raw_after = timeRawRtc();
  g_state.last_ntp_rtc = raw_after != 0 ? raw_after : true_utc;
  g_state.last_ntp_utc = true_utc;
}
