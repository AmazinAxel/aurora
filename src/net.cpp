#include "net.h"

#include <WiFi.h>
#include <esp_sntp.h>
#include <freertos/event_groups.h>
#include <nvs_flash.h>
#include <time.h>

#include "../AuroraSettings.h"
#include "power.h"
#include "state.h"

// -Wl,--wrap=nvs_flash_init: initArduino calls this every wake, to scan a
// partition nothing but WiFi reads.
extern "C" esp_err_t __real_nvs_flash_init();
extern "C" esp_err_t __wrap_nvs_flash_init() { return ESP_OK; }

namespace {

// 2024-01-01. SNTP can hand back a bogus early timestamp if it half-completes,
// and accepting one would wreck the drift model far worse than failing.
constexpr uint32_t kMinPlausibleEpoch = 1704067200UL;

constexpr EventBits_t kWifiSettled = BIT0;  // connected, or definitely won't
constexpr EventBits_t kNtpSynced = BIT1;

EventGroupHandle_t g_events = nullptr;

// The core sets WiFi.status() before running these callbacks.
void onWifiEvent(WiFiEvent_t event) {
  if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    xEventGroupSetBits(g_events, kWifiSettled);
  } else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    // The network is out of range or rejected the password: waiting out the
    // timeout would only burn the radio at full clock. The next plug-in
    // retries. Any other disconnect, the core is still retrying.
    wl_status_t st = WiFi.status();
    if (st == WL_NO_SSID_AVAIL || st == WL_CONNECT_FAILED) {
      xEventGroupSetBits(g_events, kWifiSettled);
    }
  }
}

void onNtpSync(struct timeval *) { xEventGroupSetBits(g_events, kNtpSynced); }

// What initArduino would have done, had the wrap below not skipped it. The PHY
// keeps its calibration in NVS, so WiFi is the one thing that wants it.
void nvsInit() {
  esp_err_t err = __real_nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
      err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase();
    __real_nvs_flash_init();
  }
}

void radioOff() {
  sntp_stop();
  // Stops and deinits the driver from any state; disconnect() can bail first.
  WiFi.mode(WIFI_OFF);
  powerSetLowClock();
}

}  // namespace

bool netSyncNtp(uint32_t &out_utc) {
  // The radio draws far more than the core, so finishing sooner and switching
  // it off beats running the CPU slowly with the transmitter powered.
  powerSetFullClock();
  nvsInit();

  g_events = xEventGroupCreate();

  const uint32_t deadline = millis() + NTP_TOTAL_TIMEOUT_MS;
  auto waitFor = [deadline](EventBits_t bit) {
    int32_t left = (int32_t)(deadline - millis());
    return (xEventGroupWaitBits(g_events, bit, pdFALSE, pdFALSE,
                                left > 0 ? pdMS_TO_TICKS(left) : 0) &
            bit) != 0;
  };

  WiFi.onEvent(onWifiEvent);
  WiFi.persistent(false);  // don't wear flash writing credentials every sync
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);  // modem sleep would delay the NTP reply
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD, g_state.wifi_channel);

  waitFor(kWifiSettled);
  bool connected = WiFi.status() == WL_CONNECTED;

  // A failure may mean the AP changed channel, so the next try scans them all.
  g_state.wifi_channel = connected ? (uint8_t)WiFi.channel() : 0;

  // Stays 0 on any failure, which the plausibility check below rejects.
  time_t now = 0;
  if (connected) {
    sntp_set_time_sync_notification_cb(onNtpSync);

    // UTC only: timezone and DST are display concerns applied elsewhere, and
    // letting the C library apply them here would corrupt the stored
    // timebase.
    configTime(0, 0, NTP_SERVER);

    if (waitFor(kNtpSynced)) {
      time(&now);
    }
  }
  radioOff();

  if ((uint32_t)now < kMinPlausibleEpoch) {
    return false;
  }
  out_utc = (uint32_t)now;
  return true;
}
