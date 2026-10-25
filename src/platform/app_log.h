#ifndef PLATFORM_APP_LOG_H
#define PLATFORM_APP_LOG_H

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  APP_LOG_LEVEL_DEBUG = 0,
  APP_LOG_LEVEL_INFO,
  APP_LOG_LEVEL_WARN,
  APP_LOG_LEVEL_ERROR
} app_log_level_t;

void app_log_set_level(app_log_level_t level);
app_log_level_t app_log_get_level(void);
bool app_log_is_enabled(app_log_level_t level);
void app_log_write(app_log_level_t level, const char* scope, const char* format, ...);
void app_log_vwrite(app_log_level_t level, const char* scope, const char* format, va_list args);
void app_log_debug(const char* scope, const char* format, ...);
void app_log_info(const char* scope, const char* format, ...);
void app_log_warn(const char* scope, const char* format, ...);
void app_log_error(const char* scope, const char* format, ...);

#ifdef __cplusplus
}
#endif

#endif
