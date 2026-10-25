#include "timezone_utils.h"

#include <strings.h>

namespace timezone_utils {
namespace {

struct zone_rule_t {
  const char* name;
  const char* posix;
};

constexpr const char* RULE_UTC = "UTC0";
constexpr const char* RULE_CENTRAL_EUROPE = "CET-1CEST,M3.5.0,M10.5.0/3";
constexpr const char* RULE_US_EASTERN = "EST5EDT,M3.2.0,M11.1.0";

const zone_rule_t ZONE_RULES[] = {
    {"Europe/Paris", RULE_CENTRAL_EUROPE},
    {"Europe/Brussels", RULE_CENTRAL_EUROPE},
    {"Europe/Luxembourg", RULE_CENTRAL_EUROPE},
    {"Europe/Monaco", RULE_CENTRAL_EUROPE},
    {"Europe/Zurich", RULE_CENTRAL_EUROPE},
    {"Europe/Berlin", RULE_CENTRAL_EUROPE},
    {"Europe/Amsterdam", RULE_CENTRAL_EUROPE},
    {"Europe/Madrid", RULE_CENTRAL_EUROPE},
    {"Europe/Rome", RULE_CENTRAL_EUROPE},
    {"Europe/London", "GMT0BST,M3.5.0/1,M10.5.0"},
    {"Europe/Lisbon", "WET0WEST,M3.5.0/1,M10.5.0"},
    {"Europe/Athens", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"America/Martinique", "AST4"},
    {"America/Guadeloupe", "AST4"},
    {"America/Cayenne", "<-03>3"},
    {"America/New_York", RULE_US_EASTERN},
    {"America/Toronto", RULE_US_EASTERN},
    {"America/Montreal", RULE_US_EASTERN},
    {"America/Los_Angeles", "PST8PDT,M3.2.0,M11.1.0"},
    {"Indian/Reunion", "<+04>-4"},
    {"Indian/Mayotte", "EAT-3"},
    {"Pacific/Noumea", "<+11>-11"},
    {"Pacific/Tahiti", "<-10>10"},
    {"Asia/Tokyo", "JST-9"},
    {"UTC", RULE_UTC},
    {"Etc/UTC", RULE_UTC},
    {"GMT", RULE_UTC},
};

}  // namespace

const char* to_posix(const char* timezone) {
  if (!timezone || timezone[0] == '\0') {
    return RULE_UTC;
  }
  for (const zone_rule_t& rule : ZONE_RULES) {
    if (strcasecmp(timezone, rule.name) == 0) {
      return rule.posix;
    }
  }
  return timezone;
}

}  // namespace timezone_utils
