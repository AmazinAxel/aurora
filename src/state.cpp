#include "state.h"

#include <string.h>

#include "../AuroraSettings.h"

RTC_DATA_ATTR PersistedState g_state;

bool stateInitIfCold() {
  if (g_state.magic == STATE_MAGIC) {
    return false;
  }

  memset(&g_state, 0, sizeof(g_state));
  g_state.magic = STATE_MAGIC;
  g_state.preset_index = POMODORO_PRESET_COUNT > 1 ? 1 : 0;
  return true;
}
