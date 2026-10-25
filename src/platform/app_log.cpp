#include "app_log.h"

#include <stdio.h>

#include "arduino_wrapper.h"

namespace {
constexpr size_t LOG_MESSAGE_BUFFER_SIZE = 192;
app_log_level_t g_log_level = APP_LOG_LEVEL_INFO;

const char* level_name(app_log_level_t level) {
  switch (level) {
    case APP_LOG_LEVEL_DEBUG: return "DEBUG";
    case APP_LOG_LEVEL_INFO: return "INFO";
    case APP_LOG_LEVEL_WARN: return "WARN";
    case APP_LOG_LEVEL_ERROR: return "ERROR";
  }
  return "INFO";
}
}  // namespace

extern "C" void app_log_set_level(app_log_level_t level) {
  g_log_level = level;
}

extern "C" app_log_level_t app_log_get_level(void) {
  return g_log_level;
}

extern "C" bool app_log_is_enabled(app_log_level_t level) {
  return level >= g_log_level;
}

extern "C" void app_log_vwrite(app_log_level_t level,
                               const char* scope,
                               const char* format,
                               va_list args) {
  if (!app_log_is_enabled(level)) {
    return;
  }

  char message[LOG_MESSAGE_BUFFER_SIZE];
  vsnprintf(message, sizeof(message), format ? format : "", args);
  serial_printf("[%10lu] %-5s %-10s %s\n",
                static_cast<unsigned long>(get_millis()),
                level_name(level),
                scope ? scope : "app",
                message);
}

extern "C" void app_log_write(app_log_level_t level, const char* scope, const char* format, ...) {
  va_list args;
  va_start(args, format);
  app_log_vwrite(level, scope, format, args);
  va_end(args);
}

extern "C" void app_log_debug(const char* scope, const char* format, ...) {
  va_list args;
  va_start(args, format);
  app_log_vwrite(APP_LOG_LEVEL_DEBUG, scope, format, args);
  va_end(args);
}

extern "C" void app_log_info(const char* scope, const char* format, ...) {
  va_list args;
  va_start(args, format);
  app_log_vwrite(APP_LOG_LEVEL_INFO, scope, format, args);
  va_end(args);
}

extern "C" void app_log_warn(const char* scope, const char* format, ...) {
  va_list args;
  va_start(args, format);
  app_log_vwrite(APP_LOG_LEVEL_WARN, scope, format, args);
  va_end(args);
}

extern "C" void app_log_error(const char* scope, const char* format, ...) {
  va_list args;
  va_start(args, format);
  app_log_vwrite(APP_LOG_LEVEL_ERROR, scope, format, args);
  va_end(args);
}
