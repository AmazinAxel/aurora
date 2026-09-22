// VBUS probe -- throwaway, not part of the firmware.
//
// Finds which GPIO (if any) tracks USB power on this board, so the watch can
// notice being plugged in and unplugged. The catch is that serial runs over
// the same USB-C being unplugged: the link dies with the power, so anything
// printed while unplugged is lost -- and writes into the dead bridge still
// report success, so there is no way to tell which ones landed. Samples are
// buffered in RAM, nothing prints on its own, and the whole buffer is
// replayed when you press a key, which proves the host is listening.
//
// Requires a charged battery connected, or unplugging is a hard power loss and
// the buffer dies with it. If a replug prints "probe ready" instead of a dump,
// that is what happened.
//
// Build:  pio run -e probe -t upload
// Watch:  pio device monitor -f send_on_enter
// Then:   press Enter to replay the buffer at any point.

#include <Arduino.h>

// RTC-capable pins only (0, 2, 4, 12-15, 25-27, 32-39), since whatever is
// found has to work as a deep-sleep wake source later. Minus the ones this
// board already uses: 4/25/26/35 buttons, 5/9/10/19 panel, 21/22 I2C,
// 13 motor, 34 battery sense, 33 the RTC crystal line.
const int kPins[] = {0, 2, 12, 14, 15, 27, 32, 36, 37, 38, 39};
constexpr int kN = sizeof(kPins) / sizeof(kPins[0]);

// 120 s of window at 500 ms. ~2.4 KB of .bss, against ~320 KB of RAM with no
// WiFi stack linked in, so there is plenty of room to raise this.
constexpr int kMaxSamples = 240;
constexpr uint32_t kIntervalMs = 500;

namespace {

uint32_t g_mask[kMaxSamples];
uint16_t g_vbat[kMaxSamples];
uint32_t g_ms[kMaxSamples];
int g_count = 0;

uint16_t readVbat() {
  // 2:1 divider on the battery sense line.
  return (uint16_t)(analogReadMilliVolts(34) * 2);
}

// Replays the entire buffer, never a "what's new since last time" slice.
// Writes made while the bridge is unpowered disappear silently but still
// return success, so tracking a print cursor loses exactly the unplugged
// window the probe exists to capture. Reprinting everything is the only
// version that survives the link dying mid-dump.
void dumpAll() {
  Serial.printf("=== %d samples ===\n", g_count);
  for (int s = 0; s < g_count; s++) {
    Serial.printf("[%6u ms] ", g_ms[s]);
    for (int i = 0; i < kN; i++) {
      Serial.printf("%d=%d ", kPins[i], (g_mask[s] >> i) & 1);
    }
    Serial.printf("| vbat=%u mV\n", g_vbat[s]);
  }
  Serial.println("=== end ===");
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(2000);

  for (int i = 0; i < kN; i++) {
    pinMode(kPins[i], INPUT);
  }

  Serial.println();
  Serial.println("probe ready -- sit ~10 s, unplug, wait ~20 s, replug");
  Serial.printf("buffer holds %d samples (%lu s)\n", kMaxSamples,
                (unsigned long)(kMaxSamples * kIntervalMs / 1000));
}

void loop() {
  // The dump is on demand, not on a timer: the probe cannot tell when the
  // host has the port open again, but a received byte proves it does.
  if (Serial.available()) {
    while (Serial.available()) Serial.read();
    dumpAll();
  }

  static uint32_t last = 0;
  if (millis() - last < kIntervalMs) return;
  last = millis();

  // Stops rather than wrapping, so the unplug window can never be overwritten
  // by later samples before it has been read out.
  if (g_count < kMaxSamples) {
    uint32_t mask = 0;
    for (int i = 0; i < kN; i++) {
      mask |= (uint32_t)digitalRead(kPins[i]) << i;
    }
    g_mask[g_count] = mask;
    g_vbat[g_count] = readVbat();
    g_ms[g_count] = millis();
    g_count++;
  }

  // Nothing printed here. Sampling stays silent so the buffer is only ever
  // read out by an explicit keypress, once the link is known to be alive.
}
