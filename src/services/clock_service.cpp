#include "clock_service.h"

#include <Arduino.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "platform/app_log.h"
#include "timezone_utils.h"

namespace {
constexpr uint32_t NTP_RETRY_MS = 30000;

void apply_timezone(const char* timezone) {
  setenv("TZ", timezone_utils::to_posix(timezone), 1);
  tzset();
}

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

ClockService::ClockService() : synced_(false), last_sync_attempt_ms_(0) {
  timezone_[0] = '\0';
}

void ClockService::begin(const char* timezone) {
  safe_copy(timezone_, sizeof(timezone_), timezone && timezone[0] ? timezone : "Europe/Paris");
  synced_ = false;
  last_sync_attempt_ms_ = 0;
  // Apply now: when the clock is already set (or offline), no NTP sync may follow.
  apply_timezone(timezone_);
  app_log_info("clock", "begin timezone=\"%s\" posix=\"%s\"", timezone_, timezone_utils::to_posix(timezone_));
}

void ClockService::set_timezone(const char* timezone) {
  begin(timezone);
}

void ClockService::update(bool network_ready) {
  const bool was_synced = synced_;
  const uint32_t now_ms = millis();
  const bool retry_due = (last_sync_attempt_ms_ == 0) || (now_ms - last_sync_attempt_ms_ >= NTP_RETRY_MS);
  if (network_ready && !synced_ && retry_due) {
    trigger_ntp_sync();
  }

  time_t now = time(nullptr);
  synced_ = now > 946684800;
  if (synced_ != was_synced) {
    app_log_info("clock", "sync state=%s", synced_ ? "synced" : "pending");
  }
}

bool ClockService::is_synced() const {
  return synced_;
}

void ClockService::format_time(char* out, size_t out_size) const {
  if (!out || out_size == 0) {
    return;
  }

  if (!synced_) {
    snprintf(out, out_size, "--:--");
    return;
  }

  time_t now = time(nullptr);
  struct tm time_info;
  localtime_r(&now, &time_info);
  snprintf(out, out_size, "%02d:%02d", time_info.tm_hour, time_info.tm_min);
}

bool ClockService::local_time(struct tm* out) const {
  if (!out || !synced_) {
    return false;
  }
  const time_t now = time(nullptr);
  localtime_r(&now, out);
  return true;
}

void ClockService::format_today(char* out, size_t out_size) const {
  if (!out || out_size == 0) {
    return;
  }
  struct tm time_info;
  if (!local_time(&time_info)) {
    out[0] = '\0';
    return;
  }
  strftime(out, out_size, "%Y-%m-%d", &time_info);
}

void ClockService::trigger_ntp_sync() {
  app_log_info("clock", "trigger ntp sync timezone=\"%s\"", timezone_);
  // newlib cannot parse IANA names such as "Europe/Paris": it would silently keep UTC.
  configTzTime(timezone_utils::to_posix(timezone_), "pool.ntp.org", "time.nist.gov", "time.google.com");
  last_sync_attempt_ms_ = millis();
}
