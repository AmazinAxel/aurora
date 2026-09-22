#pragma once

#include <stdint.h>

// Brings WiFi up, queries NTP, and tears the radio down on every exit path --
// success, timeout, association or DNS failure. WiFi is by far the largest
// draw on this board, so it is off at all times outside this call. Bounded by
// NTP_TOTAL_TIMEOUT_MS regardless of how the network misbehaves.
bool netSyncNtp(uint32_t &out_utc);
