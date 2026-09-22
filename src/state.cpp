#include "state.h"

#include "../AuroraSettings.h"

RTC_DATA_ATTR PersistedState g_state;

bool stateInitIfCold() {
  if (g_state.magic == STATE_MAGIC) {
    return false;
  }

  memset(&g_state, 0, sizeof(g_state));
  g_state.magic = STATE_MAGIC;
  g_state.preset_index = POMODORO_PRESET_COUNT > 1 ? 1 : 0;
  g_state.partial_count = FULL_REFRESH_INTERVAL;
  return true;
}
