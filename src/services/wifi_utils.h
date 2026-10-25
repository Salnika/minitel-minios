#ifndef SERVICES_WIFI_UTILS_H
#define SERVICES_WIFI_UTILS_H

namespace wifi_utils {

// What the radio does after a connect that failed or was cancelled.
enum after_connect_t {
  AFTER_CONNECT_STAY = 0,       // leave it trying the network asked for
  AFTER_CONNECT_BACK_TO_SAVED,  // reconnect to the saved network
  AFTER_CONNECT_STOP,           // cancelled with no saved network to go back to: stop trying
};

// `ssid` is the network asked for, `saved` the saved one ("" for none).
after_connect_t after_failed_connect(const char* ssid, const char* saved, bool cancelled);

}  // namespace wifi_utils

#endif
