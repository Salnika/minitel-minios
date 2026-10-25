#include "wifi_service.h"

#include <stdio.h>
#include <string.h>

#include "app/lang.h"
#include "platform/app_log.h"

namespace {
void safe_copy(char* dest, size_t dest_size, const char* src) {
  if (!dest || dest_size == 0) {
    return;
  }
  if (!src) {
    dest[0] = '\0';
    return;
  }
  strncpy(dest, src, dest_size - 1);
  dest[dest_size - 1] = '\0';
}
}  // namespace

void WifiService::begin() {
  app_log_info("wifi", "init");
  lm_wifi_init();
}

int WifiService::scan(lm_wifi_network_t* out, int max_items) {
  const int count = lm_wifi_scan(out, max_items);
  app_log_info("wifi", "scan count=%d", count);
  // WiFi.scanNetworks() reports failures as negative codes (-2 while a connect is running).
  return count < 0 ? 0 : count;
}

bool WifiService::connect(const char* ssid, const char* password, bool (*cancelled)()) {
  if (!ssid || ssid[0] == '\0') {
    app_log_warn("wifi", "connect request without ssid");
    return false;
  }

  app_log_info("wifi",
               "connect ssid=\"%s\" password=%s",
               ssid,
               (password && password[0] != '\0') ? "set" : "empty");
  if (!lm_wifi_connect_cancellable(ssid, password, cancelled)) {
    // Cancelled: the radio may still connect, nobody waits for it.
    app_log_warn("wifi", "connect result=%s ssid=\"%s\"", cancelled && cancelled() ? "cancelled" : "ko", ssid);
    return false;
  }
  app_log_info("wifi", "connect result=ok ssid=\"%s\"", ssid);
  return true;
}

void WifiService::disconnect() {
  app_log_info("wifi", "disconnect");
  lm_wifi_disconnect();
}

bool WifiService::is_connected() const {
  return lm_wifi_is_connected();
}

// Said here rather than by lib_minitel, which only knows French.
void WifiService::get_ssid(char* out, size_t out_size) const {
  if (!is_connected()) {
    safe_copy(out, out_size, TR("Non connecte", "Not connected"));
    return;
  }
  lm_wifi_get_ssid(out, out_size);
}

void WifiService::get_ip(char* out, size_t out_size) const {
  lm_wifi_get_ip(out, out_size);
}

void WifiService::get_status_label(char* out, size_t out_size) const {
  if (!out || out_size == 0) {
    return;
  }

  if (!is_connected()) {
    safe_copy(out, out_size, "offline");
    return;
  }

  char ssid[SETTINGS_MAX_WIFI_SSID + 1];
  get_ssid(ssid, sizeof(ssid));
  safe_copy(out, out_size, ssid);
}
