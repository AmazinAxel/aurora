#include "net.h"

#include <WiFi.h>
#include <esp_sntp.h>
#include <esp_wifi.h>
#include <time.h>

#include "../AuroraSettings.h"
#include "power.h"

namespace {

// 2024-01-01. SNTP can hand back a bogus early timestamp if it half-completes,
// and accepting one would wreck the drift model far worse than failing.
constexpr uint32_t kMinPlausibleEpoch = 1704067200UL;

void radioOff() {
  sntp_stop();
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
  esp_wifi_stop();
  esp_wifi_deinit();
  powerSetLowClock();
}

}  // namespace

bool netSyncNtp(uint32_t &out_utc) {
  // The radio draws far more than the core, so finishing sooner and switching
  // it off beats running the CPU slowly with the transmitter powered.
  powerSetFullClock();

  const uint32_t deadline = millis() + NTP_TOTAL_TIMEOUT_MS;

  WiFi.persistent(false);  // don't wear flash writing credentials every sync
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  for (wl_status_t st; (st = WiFi.status()) != WL_CONNECTED;) {
    // The network is out of range or rejected the password: waiting out the
    // timeout would only burn the radio at full clock. The next unplug retries.
    if (st == WL_NO_SSID_AVAIL || st == WL_CONNECT_FAILED ||
        (int32_t)(millis() - deadline) >= 0) {
      radioOff();
      return false;
    }
    delay(50);
  }

  // UTC only: timezone and DST are display concerns applied elsewhere, and
  // letting the C library apply them here would corrupt the stored timebase.
  configTime(0, 0, NTP_SERVER);

  while (sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED) {
    if ((int32_t)(millis() - deadline) >= 0) {
      radioOff();
      return false;
    }
    delay(50);
  }

  time_t now = 0;
  time(&now);
  radioOff();

  if ((uint32_t)now < kMinPlausibleEpoch) {
    return false;
  }
  out_utc = (uint32_t)now;
  return true;
}
