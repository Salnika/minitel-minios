#include "wifi_utils.h"

#include <string.h>

namespace wifi_utils {

after_connect_t after_failed_connect(const char* ssid, const char* saved, bool cancelled) {
  const bool has_saved = saved && saved[0] != '\0';
  if (has_saved && (!ssid || strcmp(ssid, saved) != 0)) {
    return AFTER_CONNECT_BACK_TO_SAVED;
  }
  // The saved network itself is retried, as at boot, and so is a new one after a plain
  // failure. A cancel means "not this one": nothing saved, nothing to retry.
  if (!has_saved && cancelled) {
    return AFTER_CONNECT_STOP;
  }
  return AFTER_CONNECT_STAY;
}

}  // namespace wifi_utils
