#ifndef SERVICES_OTA_SERVICE_H
#define SERVICES_OTA_SERVICE_H

#include <stddef.h>
#include <stdint.h>

// Firmware updates, two ways:
// - pushed from a computer on the same network (ArduinoOTA: `pio run -e ota -t upload`);
// - pulled from the Bibliotek server (`/firmware/manifest`, `/firmware/download`).
class OtaService {
public:
  // `message` is a short French status; `percent` is -1 when it does not apply.
  typedef void (*progress_fn)(const char* message, int percent);

  OtaService();

  // Starts the network listener once Wi-Fi is up. Without a password it stays off:
  // anyone on the network could flash the device otherwise.
  void begin_lan(const char* hostname, const char* password, progress_fn on_progress);
  void handle();
  bool lan_enabled() const;
  const char* hostname() const;

  // Reads the server manifest; `out_newer` tells whether it beats the running firmware.
  bool check_server(const char* base_url,
                    const char* token,
                    char* out_version,
                    size_t out_version_size,
                    bool* out_newer);
  // Downloads and flashes the server firmware (MD5-checked), then reboots. `should_stop`
  // (may be null) is polled during the download: true aborts it.
  bool install_from_server(const char* base_url,
                           const char* token,
                           progress_fn on_progress,
                           bool (*should_stop)());
  const char* last_error() const;

private:
  char hostname_[32];
  char last_error_[48];
  bool lan_started_;

  void set_error(const char* message);
};

#endif
