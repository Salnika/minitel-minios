#ifndef SERVICES_SETTINGS_SERVICE_H
#define SERVICES_SETTINGS_SERVICE_H

#include <stddef.h>

constexpr size_t SETTINGS_MAX_WIFI_SSID = 32;
constexpr size_t SETTINGS_MAX_WIFI_PASSWORD = 64;
constexpr size_t SETTINGS_MAX_API_URL = 96;
constexpr size_t SETTINGS_MAX_API_TOKEN = 96;
constexpr size_t SETTINGS_MAX_AI_BASE_URL = 128;
constexpr size_t SETTINGS_MAX_AI_API_KEY = 192;
constexpr size_t SETTINGS_MAX_AI_MODEL = 48;
constexpr size_t SETTINGS_MAX_TIMEZONE = 48;
constexpr size_t SETTINGS_MAX_LAST_DOC = 96;

struct app_settings_t {
  char wifi_ssid[SETTINGS_MAX_WIFI_SSID + 1];
  char wifi_password[SETTINGS_MAX_WIFI_PASSWORD + 1];
  char api_url[SETTINGS_MAX_API_URL + 1];
  char api_token[SETTINGS_MAX_API_TOKEN + 1];
  char ai_base_url[SETTINGS_MAX_AI_BASE_URL + 1];
  char ai_api_key[SETTINGS_MAX_AI_API_KEY + 1];
  char ai_model[SETTINGS_MAX_AI_MODEL + 1];
  char timezone[SETTINGS_MAX_TIMEZONE + 1];
  char last_document_id[SETTINGS_MAX_LAST_DOC + 1];
  // The chat goes to Bibliotek's assistant, which may read and change the notes. On by
  // default; without Docs URL and token the chat stays direct anyway.
  bool ai_tools;
};

class SettingsService {
public:
  SettingsService();
  void begin();
  void load(app_settings_t* settings);
  void save(const app_settings_t& settings);
  void reset();

  // Small extra values (PIN, timer, recent documents). Keys are at most 15 characters.
  void store_value(const char* key, const char* value);
  void load_value(const char* key, char* out, size_t out_size);
  void store_secret_value(const char* key, const char* value);
  void load_secret_value(const char* key, char* out, size_t out_size);

private:
  bool started_;

  void read_string(const char* key, char* out, size_t out_size);
  void read_secret(const char* key, char* out, size_t out_size);
  void write_string(const char* key, const char* value);
  void write_secret(const char* key, const char* value);
};

#endif
