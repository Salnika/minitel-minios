#include "settings_service.h"

#include <Preferences.h>
#include <stdio.h>
#include <string.h>

#include "platform/app_log.h"

#if __has_include("../app/local_secrets.h")
#include "../app/local_secrets.h"
#else
#define LOCAL_DEFAULT_WIFI_SSID ""
#define LOCAL_DEFAULT_WIFI_PASSWORD ""
#define LOCAL_DEFAULT_API_URL ""
#define LOCAL_DEFAULT_API_TOKEN ""
#define LOCAL_DEFAULT_AI_BASE_URL "https://inference.do-ai.run/v1"
#define LOCAL_DEFAULT_AI_API_KEY ""
#define LOCAL_DEFAULT_AI_MODEL "deepseek-v4.1-flash"
#endif

#ifndef LOCAL_DEFAULT_AI_BASE_URL
#define LOCAL_DEFAULT_AI_BASE_URL "https://inference.do-ai.run/v1"
#endif

#ifndef LOCAL_DEFAULT_AI_API_KEY
#define LOCAL_DEFAULT_AI_API_KEY ""
#endif

#ifndef LOCAL_DEFAULT_AI_MODEL
#define LOCAL_DEFAULT_AI_MODEL "deepseek-v4.1-flash"
#endif

namespace {
Preferences g_preferences;
constexpr const char* PREF_NAMESPACE = "minios";
constexpr const char* OBFUSCATION_KEY = "miniOS-telic";
constexpr const char* DEFAULT_TIMEZONE = "Europe/Paris";
constexpr const char* LEGACY_DEFAULT_API_URL = "http://minios-api.local";
// Settings layout version. 2: the former default model is replaced by a stronger one.
constexpr uint32_t SETTINGS_VERSION = 2;
constexpr const char* SETTINGS_VERSION_KEY = "cfg_ver";
constexpr const char* LEGACY_DEFAULT_AI_MODEL = "openai-gpt-oss-120b";
constexpr const char* UPGRADED_DEFAULT_AI_MODEL = "deepseek-v4.1-flash";
// Secrets are stored hex-encoded (2 chars per byte): sized for the longest one, the AI key.
constexpr size_t SECRET_HEX_BUFFER_SIZE = 2 * SETTINGS_MAX_AI_API_KEY + 1;

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

void hex_encode(const char* input, char* output, size_t output_size) {
  if (!output || output_size == 0) {
    return;
  }
  output[0] = '\0';

  if (!input) {
    return;
  }

  size_t key_len = strlen(OBFUSCATION_KEY);
  size_t out_index = 0;
  for (size_t i = 0; input[i] != '\0' && out_index + 2 < output_size; ++i) {
    uint8_t encoded = static_cast<uint8_t>(input[i]) ^ static_cast<uint8_t>(OBFUSCATION_KEY[i % key_len]);
    snprintf(output + out_index, output_size - out_index, "%02X", encoded);
    out_index += 2;
  }
}

void hex_decode(const char* input, char* output, size_t output_size) {
  if (!output || output_size == 0) {
    return;
  }
  output[0] = '\0';

  if (!input) {
    return;
  }

  size_t key_len = strlen(OBFUSCATION_KEY);
  size_t input_len = strlen(input);
  size_t out_index = 0;
  for (size_t i = 0; i + 1 < input_len && out_index + 1 < output_size; i += 2) {
    unsigned int value = 0;
    if (sscanf(input + i, "%02X", &value) != 1) {
      break;
    }
    output[out_index] = static_cast<char>(static_cast<uint8_t>(value) ^
                                          static_cast<uint8_t>(OBFUSCATION_KEY[out_index % key_len]));
    ++out_index;
  }
  output[out_index] = '\0';
}

bool string_equals(const char* a, const char* b) {
  if (!a || !b) {
    return false;
  }
  return strcmp(a, b) == 0;
}
}  // namespace

SettingsService::SettingsService() : started_(false) {}

void SettingsService::begin() {
  if (!started_) {
    g_preferences.begin(PREF_NAMESPACE, false);
    started_ = true;
    app_log_info("settings", "preferences opened namespace=\"%s\"", PREF_NAMESPACE);
  }
}

void SettingsService::load(app_settings_t* settings) {
  if (!settings) {
    return;
  }

  memset(settings, 0, sizeof(*settings));
  safe_copy(settings->timezone, sizeof(settings->timezone), DEFAULT_TIMEZONE);

  read_string("wifi_ssid", settings->wifi_ssid, sizeof(settings->wifi_ssid));
  read_secret("wifi_pass", settings->wifi_password, sizeof(settings->wifi_password));
  read_string("api_url", settings->api_url, sizeof(settings->api_url));
  read_secret("api_token", settings->api_token, sizeof(settings->api_token));
  read_string("ai_url", settings->ai_base_url, sizeof(settings->ai_base_url));
  read_secret("ai_key", settings->ai_api_key, sizeof(settings->ai_api_key));
  read_string("ai_model", settings->ai_model, sizeof(settings->ai_model));
  read_string("timezone", settings->timezone, sizeof(settings->timezone));
  read_string("last_doc", settings->last_document_id, sizeof(settings->last_document_id));
  char ai_tools[2] = "";
  read_string("ai_tools", ai_tools, sizeof(ai_tools));
  settings->ai_tools = ai_tools[0] != '0';

  // The default password belongs to the default network only: an open network saved with
  // an empty password must not inherit it.
  if (settings->wifi_ssid[0] == '\0') {
    safe_copy(settings->wifi_ssid, sizeof(settings->wifi_ssid), LOCAL_DEFAULT_WIFI_SSID);
    safe_copy(settings->wifi_password, sizeof(settings->wifi_password), LOCAL_DEFAULT_WIFI_PASSWORD);
  } else if (settings->wifi_password[0] == '\0' &&
             string_equals(settings->wifi_ssid, LOCAL_DEFAULT_WIFI_SSID)) {
    safe_copy(settings->wifi_password, sizeof(settings->wifi_password), LOCAL_DEFAULT_WIFI_PASSWORD);
  }
  if (settings->api_url[0] == '\0') {
    safe_copy(settings->api_url, sizeof(settings->api_url), LOCAL_DEFAULT_API_URL);
  }
  if (settings->api_token[0] == '\0') {
    safe_copy(settings->api_token, sizeof(settings->api_token), LOCAL_DEFAULT_API_TOKEN);
  }
  if (settings->ai_base_url[0] == '\0') {
    safe_copy(settings->ai_base_url, sizeof(settings->ai_base_url), LOCAL_DEFAULT_AI_BASE_URL);
  }
  if (settings->ai_api_key[0] == '\0') {
    safe_copy(settings->ai_api_key, sizeof(settings->ai_api_key), LOCAL_DEFAULT_AI_API_KEY);
  }
  if (settings->ai_model[0] == '\0') {
    safe_copy(settings->ai_model, sizeof(settings->ai_model), LOCAL_DEFAULT_AI_MODEL);
  }
  if (settings->timezone[0] == '\0') {
    safe_copy(settings->timezone, sizeof(settings->timezone), DEFAULT_TIMEZONE);
  }

  const uint32_t stored_version = started_ ? g_preferences.getUInt(SETTINGS_VERSION_KEY, 1) : SETTINGS_VERSION;
  if (stored_version < SETTINGS_VERSION) {
    if (string_equals(settings->ai_model, LEGACY_DEFAULT_AI_MODEL)) {
      safe_copy(settings->ai_model, sizeof(settings->ai_model), UPGRADED_DEFAULT_AI_MODEL);
      write_string("ai_model", settings->ai_model);
      app_log_warn("settings", "ai model upgraded to \"%s\"", settings->ai_model);
    }
    if (started_) {
      g_preferences.putUInt(SETTINGS_VERSION_KEY, SETTINGS_VERSION);
    }
  }

  const bool should_migrate_legacy_api =
      string_equals(settings->api_url, LEGACY_DEFAULT_API_URL) &&
      LOCAL_DEFAULT_API_URL[0] != '\0' &&
      !string_equals(LOCAL_DEFAULT_API_URL, LEGACY_DEFAULT_API_URL);
  if (should_migrate_legacy_api) {
    safe_copy(settings->api_url, sizeof(settings->api_url), LOCAL_DEFAULT_API_URL);
    if (LOCAL_DEFAULT_API_TOKEN[0] != '\0') {
      safe_copy(settings->api_token, sizeof(settings->api_token), LOCAL_DEFAULT_API_TOKEN);
    }
    write_string("api_url", settings->api_url);
    write_secret("api_token", settings->api_token);
    app_log_warn("settings", "migrated legacy api settings to local defaults");
  }

  app_log_info("settings",
               "load wifi_ssid=\"%s\" api_url=\"%s\" ai_url=\"%s\" ai_model=\"%s\" ai_tools=%s timezone=\"%s\" "
               "last_doc=\"%s\" password=%s token=%s ai_key=%s",
               settings->wifi_ssid[0] ? settings->wifi_ssid : "<empty>",
               settings->api_url[0] ? settings->api_url : "<empty>",
               settings->ai_base_url[0] ? settings->ai_base_url : "<empty>",
               settings->ai_model[0] ? settings->ai_model : "<empty>",
               settings->ai_tools ? "on" : "off",
               settings->timezone[0] ? settings->timezone : "<empty>",
               settings->last_document_id[0] ? settings->last_document_id : "<empty>",
               settings->wifi_password[0] ? "set" : "empty",
               settings->api_token[0] ? "set" : "empty",
               settings->ai_api_key[0] ? "set" : "empty");
}

void SettingsService::save(const app_settings_t& settings) {
  write_string("wifi_ssid", settings.wifi_ssid);
  write_secret("wifi_pass", settings.wifi_password);
  write_string("api_url", settings.api_url);
  write_secret("api_token", settings.api_token);
  write_string("ai_url", settings.ai_base_url);
  write_secret("ai_key", settings.ai_api_key);
  write_string("ai_model", settings.ai_model);
  write_string("timezone", settings.timezone);
  write_string("last_doc", settings.last_document_id);
  write_string("ai_tools", settings.ai_tools ? "1" : "0");
  app_log_info("settings",
               "save wifi_ssid=\"%s\" api_url=\"%s\" ai_url=\"%s\" ai_model=\"%s\" ai_tools=%s timezone=\"%s\" "
               "last_doc=\"%s\" password=%s token=%s ai_key=%s",
               settings.wifi_ssid[0] ? settings.wifi_ssid : "<empty>",
               settings.api_url[0] ? settings.api_url : "<empty>",
               settings.ai_base_url[0] ? settings.ai_base_url : "<empty>",
               settings.ai_model[0] ? settings.ai_model : "<empty>",
               settings.ai_tools ? "on" : "off",
               settings.timezone[0] ? settings.timezone : "<empty>",
               settings.last_document_id[0] ? settings.last_document_id : "<empty>",
               settings.wifi_password[0] ? "set" : "empty",
               settings.api_token[0] ? "set" : "empty",
               settings.ai_api_key[0] ? "set" : "empty");
}

void SettingsService::reset() {
  if (!started_) {
    return;
  }
  g_preferences.clear();
  app_log_warn("settings", "reset all preferences");
}

void SettingsService::store_value(const char* key, const char* value) {
  write_string(key, value);
}

void SettingsService::load_value(const char* key, char* out, size_t out_size) {
  if (out && out_size > 0) {
    out[0] = '\0';
  }
  read_string(key, out, out_size);
}

void SettingsService::store_secret_value(const char* key, const char* value) {
  write_secret(key, value);
}

void SettingsService::load_secret_value(const char* key, char* out, size_t out_size) {
  if (out && out_size > 0) {
    out[0] = '\0';
  }
  read_secret(key, out, out_size);
}

void SettingsService::read_string(const char* key, char* out, size_t out_size) {
  if (!started_ || !key || !out || out_size == 0) {
    return;
  }
  String value = g_preferences.getString(key, "");
  safe_copy(out, out_size, value.c_str());
}

void SettingsService::read_secret(const char* key, char* out, size_t out_size) {
  char encoded[SECRET_HEX_BUFFER_SIZE] = "";
  read_string(key, encoded, sizeof(encoded));
  if (encoded[0] == '\0') {
    out[0] = '\0';
    return;
  }
  hex_decode(encoded, out, out_size);
}

void SettingsService::write_string(const char* key, const char* value) {
  if (!started_ || !key) {
    return;
  }
  g_preferences.putString(key, value ? value : "");
}

void SettingsService::write_secret(const char* key, const char* value) {
  char encoded[SECRET_HEX_BUFFER_SIZE];
  hex_encode(value ? value : "", encoded, sizeof(encoded));
  write_string(key, encoded);
}
