#pragma once

#include <stdint.h>

// The only place the panel is driven, so the ink policy cannot be bypassed.
// Partial refresh by default; full on a forced draw, on the cover, and
// whenever the partial counter reaches FULL_REFRESH_INTERVAL.

void displayWatchface(uint32_t now_utc, bool force_full);
void displayCover();
void displayHibernate();
