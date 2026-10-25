#include "ota_service.h"

#include <Arduino.h>
#include <ArduinoOTA.h>
#include <HTTPClient.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <stdio.h>
#include <string.h>

#include "app/version.h"
#include "ota_utils.h"
#include "platform/app_log.h"
#include "tls_roots.h"

namespace {
constexpr uint16_t OTA_HTTP_TIMEOUT_MS = 15000;
constexpr uint32_t OTA_STALL_TIMEOUT_MS = 20000;
// Each progress line costs ~0.2 s at 1200 baud: report every 5% only.
constexpr int OTA_PROGRESS_STEP = 5;

OtaService::progress_fn g_lan_progress = nullptr;
int g_lan_reported_percent = -1;  // network updates run on the loop only

void safe_copy(char* dest, size_t dest_size, const char* src) {
  if (!dest || dest_size == 0) {
    return;
  }
  strncpy(dest, src ? src : "", dest_size - 1);
  dest[dest_size - 1] = '\0';
}

void report(OtaService::progress_fn on_progress, int* last_percent, const char* message, int percent) {
  *last_percent = percent;
  if (on_progress) {
    on_progress(message, percent);
  }
}

void report_step(OtaService::progress_fn on_progress, int* last_percent, const char* message, int percent) {
  if (percent < 100 && *last_percent >= 0 && percent - *last_percent < OTA_PROGRESS_STEP) {
    return;
  }
  report(on_progress, last_percent, message, percent);
}

// Opens `path` on the Bibliotek server with the bearer token, over TLS for https URLs.
// The clients belong to the caller so that they outlive `http`.
bool begin_server_request(HTTPClient& http,
                          WiFiClient& plain_client,
                          WiFiClientSecure& secure_client,
                          const char* base_url,
                          const char* path,
                          const char* token) {
  char url[128];
  if (!ota_utils::server_url(base_url, path, url, sizeof(url))) {
    return false;
  }
  const bool use_tls = url_uses_tls(url);
  if (use_tls) {
    secure_client.setCACert(TLS_ROOT_CERTS);
    secure_client.setHandshakeTimeout(TLS_HANDSHAKE_TIMEOUT_S);
  }
  WiFiClient& client = use_tls ? static_cast<WiFiClient&>(secure_client) : plain_client;
  if (!http.begin(client, url)) {
    return false;
  }
  http.setTimeout(OTA_HTTP_TIMEOUT_MS);
  if (token && token[0] != '\0') {
    http.addHeader("Authorization", String("Bearer ") + token);
  }
  return true;
}

}  // namespace

OtaService::OtaService() : lan_started_(false) {
  hostname_[0] = '\0';
  last_error_[0] = '\0';
}

void OtaService::begin_lan(const char* hostname, const char* password, progress_fn on_progress) {
  if (!password || password[0] == '\0') {
    app_log_warn("ota", "lan updates off: no password (docs API token)");
    return;
  }
  if (lan_started_) {
    ArduinoOTA.end();  // Wi-Fi came back: rebind the listener
  }

  safe_copy(hostname_, sizeof(hostname_), hostname);
  g_lan_progress = on_progress;
  ArduinoOTA.setHostname(hostname_);
  ArduinoOTA.setPassword(password);
  ArduinoOTA.onStart([]() {
    g_lan_reported_percent = -1;
    report(g_lan_progress, &g_lan_reported_percent, "Mise a jour reseau", 0);
  });
  ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
    report_step(g_lan_progress, &g_lan_reported_percent, "Mise a jour reseau",
                total > 0 ? static_cast<int>(done * 100ULL / total) : 0);
  });
  ArduinoOTA.onEnd([]() { report(g_lan_progress, &g_lan_reported_percent, "Mise a jour OK, redemarrage", 100); });
  ArduinoOTA.onError([](ota_error_t error) {
    report(g_lan_progress,
           &g_lan_reported_percent,
           error == OTA_AUTH_ERROR ? "Mise a jour refusee: mot de passe" : "Mise a jour echouee",
           -1);
  });
  ArduinoOTA.begin();
  lan_started_ = true;
  app_log_info("ota", "lan updates on host=%s.local", hostname_);
}

void OtaService::handle() {
  if (lan_started_) {
    ArduinoOTA.handle();
  }
}

bool OtaService::lan_enabled() const {
  return lan_started_;
}

const char* OtaService::hostname() const {
  return hostname_;
}

bool OtaService::check_server(const char* base_url,
                              const char* token,
                              char* out_version,
                              size_t out_version_size,
                              bool* out_newer) {
  if (out_newer) {
    *out_newer = false;
  }
  if (!base_url || base_url[0] == '\0') {
    set_error("API URL vide");
    return false;
  }
  if (WiFi.status() != WL_CONNECTED) {
    set_error("WiFi hors ligne");
    return false;
  }

  WiFiClient plain_client;
  WiFiClientSecure secure_client;
  HTTPClient http;
  if (!begin_server_request(http, plain_client, secure_client, base_url, "/firmware/manifest", token)) {
    set_error("Serveur inaccessible");
    return false;
  }
  http.addHeader("Accept", "application/json");
  const int status_code = http.GET();
  const String body = status_code > 0 ? http.getString() : String();
  http.end();

  if (status_code == 404) {
    if (ota_utils::nothing_published(body.c_str())) {
      // Nothing to install: up to date as far as this server goes (empty version).
      safe_copy(out_version, out_version_size, "");
      last_error_[0] = '\0';
      app_log_info("ota", "server has no firmware, running=%s", MINIOS_FIRMWARE_VERSION);
      return true;
    }
    set_error("Pas de /firmware: serveur a redeployer");
    return false;
  }
  if (status_code != 200) {
    char message[sizeof(last_error_)];
    if (status_code > 0) {
      snprintf(message, sizeof(message), "Serveur HTTP %d", status_code);
    } else {
      snprintf(message, sizeof(message), "Serveur injoignable");
    }
    set_error(message);
    return false;
  }

  ota_utils::ota_manifest_t manifest;
  if (!ota_utils::parse_manifest(body.c_str(), &manifest)) {
    set_error("Manifeste invalide");
    return false;
  }
  safe_copy(out_version, out_version_size, manifest.version);
  if (out_newer) {
    *out_newer = ota_utils::compare_versions(manifest.version, MINIOS_FIRMWARE_VERSION) > 0;
  }
  last_error_[0] = '\0';
  app_log_info("ota", "server version=%s running=%s", manifest.version, MINIOS_FIRMWARE_VERSION);
  return true;
}

bool OtaService::install_from_server(const char* base_url,
                                     const char* token,
                                     progress_fn on_progress,
                                     bool (*should_stop)()) {
  if (!base_url || base_url[0] == '\0') {
    set_error("API URL vide");
    return false;
  }
  if (WiFi.status() != WL_CONNECTED) {
    set_error("WiFi hors ligne");
    return false;
  }

  // The manifest carries the MD5 that Update checks before switching partitions.
  ota_utils::ota_manifest_t manifest;
  {
    WiFiClient plain_client;
    WiFiClientSecure secure_client;
    HTTPClient http;
    if (!begin_server_request(http, plain_client, secure_client, base_url, "/firmware/manifest", token)) {
      set_error("Serveur inaccessible");
      return false;
    }
    const int status_code = http.GET();
    const String body = status_code == 200 ? http.getString() : String();
    http.end();
    if (status_code != 200 || !ota_utils::parse_manifest(body.c_str(), &manifest)) {
      set_error("Manifeste illisible");
      return false;
    }
  }

  WiFiClient plain_client;
  WiFiClientSecure secure_client;
  HTTPClient http;
  if (!begin_server_request(http, plain_client, secure_client, base_url, "/firmware/download", token)) {
    set_error("Serveur inaccessible");
    return false;
  }
  const int status_code = http.GET();
  if (status_code != 200) {
    http.end();
    set_error(status_code == 404 ? "Firmware absent du serveur" : "Telechargement refuse");
    return false;
  }
  const int length = http.getSize();
  if (length <= 0) {
    http.end();
    set_error("Taille du firmware inconnue");
    return false;
  }
  if (!Update.begin(static_cast<size_t>(length))) {
    http.end();
    set_error("Place insuffisante en flash");
    return false;
  }
  if (manifest.md5[0] != '\0') {
    Update.setMD5(manifest.md5);
  }

  WiFiClient* stream = http.getStreamPtr();
  uint8_t buffer[1024];
  size_t written = 0;
  uint32_t last_data_ms = millis();
  int last_percent = -1;
  report(on_progress, &last_percent, "Telechargement", 0);
  while (written < static_cast<size_t>(length)) {
    if (should_stop && should_stop()) {
      Update.abort();
      http.end();
      set_error("Mise a jour annulee");
      return false;
    }
    const size_t available = stream->available();
    if (available == 0) {
      if (!http.connected() || millis() - last_data_ms > OTA_STALL_TIMEOUT_MS) {
        break;
      }
      delay(1);
      continue;
    }
    const size_t chunk = available < sizeof(buffer) ? available : sizeof(buffer);
    const int read = stream->readBytes(buffer, chunk);
    if (read <= 0) {
      continue;
    }
    if (Update.write(buffer, static_cast<size_t>(read)) != static_cast<size_t>(read)) {
      Update.abort();
      http.end();
      set_error("Ecriture flash echouee");
      return false;
    }
    written += static_cast<size_t>(read);
    last_data_ms = millis();
    report_step(on_progress, &last_percent, "Telechargement",
                static_cast<int>(written * 100ULL / static_cast<size_t>(length)));
  }
  http.end();

  if (written != static_cast<size_t>(length)) {
    Update.abort();
    set_error("Telechargement interrompu");
    return false;
  }
  if (!Update.end(true)) {
    set_error(Update.getError() == UPDATE_ERROR_MD5 ? "Firmware corrompu (MD5)" : "Installation echouee");
    return false;
  }

  app_log_info("ota", "server update installed version=%s", manifest.version);
  report(on_progress, &last_percent, "Installe, redemarrage", 100);
  delay(500);
  ESP.restart();
  return true;
}

const char* OtaService::last_error() const {
  return last_error_;
}

void OtaService::set_error(const char* message) {
  safe_copy(last_error_, sizeof(last_error_), message);
  app_log_warn("ota", "%s", last_error_);
}
