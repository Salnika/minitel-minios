#ifndef SERVICES_CLOCK_SERVICE_H
#define SERVICES_CLOCK_SERVICE_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

class ClockService {
public:
  ClockService();

  void begin(const char* timezone);
  void set_timezone(const char* timezone);
  void update(bool network_ready);
  bool is_synced() const;
  void format_time(char* out, size_t out_size) const;
  // Local time; false until NTP has set the clock.
  bool local_time(struct tm* out) const;
  // Today as YYYY-MM-DD, or "" until NTP has set the clock.
  void format_today(char* out, size_t out_size) const;

private:
  char timezone_[49];
  bool synced_;
  uint32_t last_sync_attempt_ms_;

  void trigger_ntp_sync();
};

#endif
