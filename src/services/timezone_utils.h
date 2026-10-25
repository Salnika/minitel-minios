#ifndef SERVICES_TIMEZONE_UTILS_H
#define SERVICES_TIMEZONE_UTILS_H

namespace timezone_utils {

// POSIX TZ rule for `timezone`. newlib only understands POSIX rules, so a known IANA name
// ("Europe/Paris", any case) is looked up in a small table; anything else is returned as is
// so a POSIX rule can be typed directly. Null or empty gives UTC.
const char* to_posix(const char* timezone);

}  // namespace timezone_utils

#endif
