#ifndef SERVICES_WIFI_SERVICE_H
#define SERVICES_WIFI_SERVICE_H

#include "settings_service.h"
#include "lib_minitel/net/wifi.h"

// scan() and connect() block for seconds: only the network task calls them (net_requests.h),
// boot included.
class WifiService {
public:
  void begin();
  int scan(lm_wifi_network_t* out, int max_items);
  // Stops waiting once `cancelled` (may be null) returns true. Saves nothing.
  bool connect(const char* ssid, const char* password, bool (*cancelled)());
  void disconnect();
  bool is_connected() const;
  void get_ssid(char* out, size_t out_size) const;
  void get_ip(char* out, size_t out_size) const;
  void get_status_label(char* out, size_t out_size) const;
};

#endif
