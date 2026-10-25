#include "workspace_utils.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "app/lang.h"
#include "services/chat_service_utils.h"

namespace workspace_utils {
namespace {

constexpr size_t TASK_INPUT_BUFFER = 257;

void copy_raw(char* out, size_t out_size, const char* text) {
  if (!out || out_size == 0) {
    return;
  }
  strncpy(out, text ? text : "", out_size - 1);
  out[out_size - 1] = '\0';
}

// Server texts are UTF-8 (accents, typographic quotes): show their ASCII transliteration.
void copy_display(char* out, size_t out_size, const char* text) {
  if (!out || out_size == 0) {
    return;
  }
  bool truncated = false;
  if (!text || !chat_service_utils::normalize_text(text, out, out_size, &truncated)) {
    out[0] = '\0';
  }
}

uint16_t read_count(const JsonVariantConst& value) {
  const uint32_t count = value.as<uint32_t>();
  return count > 65535U ? 65535U : static_cast<uint16_t>(count);
}

bool is_leap_year(int year) {
  return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

int days_in_month(int year, int month) {
  static const int DAYS[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  return month == 2 && is_leap_year(year) ? 29 : DAYS[month - 1];
}

bool valid_ymd(int year, int month, int day) {
  return year >= 1970 && year <= 2999 && month >= 1 && month <= 12 && day >= 1 &&
         day <= days_in_month(year, month);
}

bool parse_iso_date(const char* text, int* year, int* month, int* day) {
  if (!text || strlen(text) != WS_DATE_LENGTH || text[4] != '-' || text[7] != '-') {
    return false;
  }
  for (size_t i = 0; i < WS_DATE_LENGTH; ++i) {
    if (i != 4 && i != 7 && !isdigit(static_cast<unsigned char>(text[i]))) {
      return false;
    }
  }
  *year = atoi(text);
  *month = atoi(text + 5);
  *day = atoi(text + 8);
  return valid_ymd(*year, *month, *day);
}

void format_iso_date(int year, int month, int day, char* out, size_t out_size) {
  snprintf(out, out_size, "%04d-%02d-%02d", year, month, day);
}

// Days since 1970-01-01 and back (proleptic Gregorian, Howard Hinnant's algorithms).
long days_from_civil(int year, int month, int day) {
  year -= month <= 2 ? 1 : 0;
  const long era = (year >= 0 ? year : year - 399) / 400;
  const long year_of_era = year - era * 400;
  const long day_of_year = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
  const long day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
  return era * 146097 + day_of_era - 719468;
}

void civil_from_days(long days, int* year, int* month, int* day) {
  days += 719468;
  const long era = (days >= 0 ? days : days - 146096) / 146097;
  const long day_of_era = days - era * 146097;
  const long year_of_era =
      (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
  const long day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
  const long month_index = (5 * day_of_year + 2) / 153;
  *day = static_cast<int>(day_of_year - (153 * month_index + 2) / 5 + 1);
  *month = static_cast<int>(month_index < 10 ? month_index + 3 : month_index - 9);
  *year = static_cast<int>(year_of_era + era * 400 + (*month <= 2 ? 1 : 0));
}

// "14:30", "14h30", "9h" -> "HH:MM".
bool parse_time_token(const char* token, char* out) {
  int hours = 0;
  int minutes = 0;
  char separator = '\0';
  int consumed = 0;
  if (sscanf(token, "%2d%c%n", &hours, &separator, &consumed) < 2 ||
      (separator != ':' && separator != 'h' && separator != 'H')) {
    return false;
  }
  const char* rest = token + consumed;
  if (*rest != '\0') {
    if (strlen(rest) != 2 || !isdigit(static_cast<unsigned char>(rest[0])) ||
        !isdigit(static_cast<unsigned char>(rest[1]))) {
      return false;
    }
    minutes = atoi(rest);
  } else if (separator == ':') {
    return false;
  }
  if (hours < 0 || hours > 23 || minutes < 0 || minutes > 59) {
    return false;
  }
  snprintf(out, WS_TIME_LENGTH + 1, "%02d:%02d", hours, minutes);
  return true;
}

// Due date after "@": "auj" or "today", "demain" or "tomorrow", "2026-10-25", "25/10",
// "25/10/27", "25/10/2027". Both languages are read whatever the build's language.
bool parse_due_token(const char* spec, const char* today, char* out) {
  int today_year = 0;
  int today_month = 0;
  int today_day = 0;
  const bool has_today = parse_iso_date(today, &today_year, &today_month, &today_day);

  if (strcasecmp(spec, "auj") == 0 || strcasecmp(spec, "aujourdhui") == 0 ||
      strcasecmp(spec, "aujourd'hui") == 0 || strcasecmp(spec, "today") == 0) {
    if (!has_today) {
      return false;
    }
    copy_raw(out, WS_DATE_LENGTH + 1, today);
    return true;
  }
  if (strcasecmp(spec, "demain") == 0 || strcasecmp(spec, "tomorrow") == 0) {
    return has_today && add_days(today, 1, out, WS_DATE_LENGTH + 1);
  }

  int year = 0;
  int month = 0;
  int day = 0;
  if (parse_iso_date(spec, &year, &month, &day)) {
    format_iso_date(year, month, day, out, WS_DATE_LENGTH + 1);
    return true;
  }

  int consumed = 0;
  if (sscanf(spec, "%2d/%2d%n", &day, &month, &consumed) != 2) {
    return false;
  }
  if (spec[consumed] == '\0') {
    // No year: the next occurrence of that day.
    if (!has_today) {
      return false;
    }
    year = today_year;
    if (valid_ymd(year, month, day) && days_from_civil(year, month, day) <
                                           days_from_civil(today_year, today_month, today_day)) {
      ++year;
    }
  } else {
    int year_digits = 0;
    if (spec[consumed] != '/' || sscanf(spec + consumed + 1, "%4d%n", &year, &year_digits) != 1 ||
        spec[consumed + 1 + year_digits] != '\0' || (year_digits != 2 && year_digits != 4)) {
      return false;
    }
    if (year_digits == 2) {
      year += 2000;
    }
  }
  if (!valid_ymd(year, month, day)) {
    return false;
  }
  format_iso_date(year, month, day, out, WS_DATE_LENGTH + 1);
  return true;
}

bool is_priority_token(const char* token) {
  const size_t length = strlen(token);
  if (length == 0 || length > 3) {
    return false;
  }
  for (size_t i = 0; i < length; ++i) {
    if (token[i] != '!') {
      return false;
    }
  }
  return true;
}

}  // namespace

int parse_projects(const JsonVariantConst& root, project_summary_t* out, int max_items) {
  JsonArrayConst items = root["items"].as<JsonArrayConst>();
  if (!out || max_items <= 0 || items.isNull()) {
    return 0;
  }

  int count = 0;
  for (JsonVariantConst item : items) {
    if (count >= max_items) {
      break;
    }
    const char* name = item["name"];
    if (!name || name[0] == '\0') {
      continue;
    }
    project_summary_t& project = out[count];
    memset(&project, 0, sizeof(project));
    copy_raw(project.name, sizeof(project.name), name);
    copy_display(project.title, sizeof(project.title), item["title"] | name);
    copy_display(project.status, sizeof(project.status), item["status"]);
    copy_display(project.goal, sizeof(project.goal), item["goal"]);
    copy_display(project.next, sizeof(project.next), item["next"]);
    project.open_tasks = read_count(item["openTasks"]);
    project.overdue_tasks = read_count(item["overdueTasks"]);
    project.week_minutes = read_count(item["weekMinutes"]);
    ++count;
  }
  return count;
}

bool parse_task(const JsonVariantConst& item, task_item_t* out) {
  if (!out) {
    return false;
  }
  memset(out, 0, sizeof(*out));
  const char* file = item["file"];
  const uint32_t line = item["line"].as<uint32_t>();
  if (!file || file[0] == '\0' || line == 0 || line > 65535U) {
    return false;
  }
  copy_raw(out->file, sizeof(out->file), file);
  out->line = static_cast<uint16_t>(line);
  copy_raw(out->project, sizeof(out->project), item["project"]);
  copy_display(out->text, sizeof(out->text), item["text"]);
  const char* due = item["due"];
  if (is_valid_date(due)) {
    copy_raw(out->due, sizeof(out->due), due);
  }
  const char* time = item["time"];
  if (time && strlen(time) == WS_TIME_LENGTH) {
    copy_raw(out->time, sizeof(out->time), time);
  }
  const uint32_t priority = item["priority"].as<uint32_t>();
  out->priority = static_cast<uint8_t>(priority > 3 ? 3 : priority);
  out->done = item["done"].as<bool>();
  return true;
}

int parse_tasks(const JsonVariantConst& root, task_item_t* out, int max_items, int* out_total) {
  JsonArrayConst items = root["items"].as<JsonArrayConst>();
  int count = 0;
  if (out && max_items > 0 && !items.isNull()) {
    for (JsonVariantConst item : items) {
      if (count >= max_items) {
        break;
      }
      if (parse_task(item, &out[count])) {
        ++count;
      }
    }
  }
  if (out_total) {
    const int total = root["total"] | count;
    *out_total = total < count ? count : total;
  }
  return count;
}

int parse_search_hits(const JsonVariantConst& root, search_hit_t* out, int max_items) {
  JsonArrayConst items = root["items"].as<JsonArrayConst>();
  if (!out || max_items <= 0 || items.isNull()) {
    return 0;
  }

  int count = 0;
  for (JsonVariantConst item : items) {
    if (count >= max_items) {
      break;
    }
    const char* id = item["id"];
    if (!id || id[0] == '\0') {
      continue;
    }
    search_hit_t& hit = out[count];
    memset(&hit, 0, sizeof(hit));
    copy_raw(hit.id, sizeof(hit.id), id);
    const uint32_t line = item["line"].as<uint32_t>();
    hit.line = static_cast<uint16_t>(line > 65535U ? 65535U : line);
    copy_display(hit.snippet, sizeof(hit.snippet), item["snippet"]);
    ++count;
  }
  return count;
}

bool parse_task_input(const char* input, const char* today, task_input_t* out) {
  if (!out) {
    return false;
  }
  memset(out, 0, sizeof(*out));
  if (!input) {
    return false;
  }

  char buffer[TASK_INPUT_BUFFER];
  copy_raw(buffer, sizeof(buffer), input);
  size_t text_length = 0;
  bool time_may_follow = false;
  char* saveptr = nullptr;
  for (char* token = strtok_r(buffer, " ", &saveptr); token; token = strtok_r(nullptr, " ", &saveptr)) {
    if (time_may_follow && parse_time_token(token, out->time)) {
      time_may_follow = false;
      continue;
    }
    time_may_follow = false;

    if (is_priority_token(token)) {
      out->priority = static_cast<uint8_t>(strlen(token));
      continue;
    }
    if (token[0] == '@' && parse_due_token(token + 1, today, out->due)) {
      time_may_follow = true;
      continue;
    }

    const size_t token_length = strlen(token);
    const size_t needed = token_length + (text_length > 0 ? 1 : 0);
    if (text_length + needed > WS_MAX_TASK_TEXT) {
      continue;  // keep what fits on the task line
    }
    if (text_length > 0) {
      out->text[text_length++] = ' ';
    }
    memcpy(out->text + text_length, token, token_length);
    text_length += token_length;
    out->text[text_length] = '\0';
  }
  return text_length > 0;
}

bool is_valid_date(const char* date) {
  int year = 0;
  int month = 0;
  int day = 0;
  return parse_iso_date(date, &year, &month, &day);
}

bool add_days(const char* date, int days, char* out, size_t out_size) {
  int year = 0;
  int month = 0;
  int day = 0;
  if (!out || out_size < WS_DATE_LENGTH + 1 || !parse_iso_date(date, &year, &month, &day)) {
    return false;
  }
  civil_from_days(days_from_civil(year, month, day) + days, &year, &month, &day);
  format_iso_date(year, month, day, out, out_size);
  return true;
}

void format_due_label(const char* due, const char* today, char* out, size_t out_size) {
  if (!out || out_size == 0) {
    return;
  }
  out[0] = '\0';
  if (!is_valid_date(due)) {
    return;
  }
  char tomorrow[WS_DATE_LENGTH + 1] = "";
  const bool has_today = is_valid_date(today);
  if (has_today && strcmp(due, today) == 0) {
    copy_raw(out, out_size, TR("auj.", "today"));
  } else if (has_today && add_days(today, 1, tomorrow, sizeof(tomorrow)) && strcmp(due, tomorrow) == 0) {
    copy_raw(out, out_size, TR("demain", "tmrw"));
  } else if (has_today && strcmp(due, today) < 0) {
    copy_raw(out, out_size, TR("retard", "late"));
  } else {
    snprintf(out, out_size, "%.2s/%.2s", due + 8, due + 5);
  }
}

void slugify(const char* input, char* out, size_t out_size) {
  if (!out || out_size == 0) {
    return;
  }
  size_t length = 0;
  bool pending_dash = false;
  for (const char* cursor = input ? input : ""; *cursor != '\0' && length + 1 < out_size; ++cursor) {
    const unsigned char ch = static_cast<unsigned char>(*cursor);
    if (isalnum(ch) && ch < 0x80) {
      if (pending_dash && length > 0 && length + 2 < out_size) {
        out[length++] = '-';
      }
      pending_dash = false;
      out[length++] = static_cast<char>(tolower(ch));
    } else {
      pending_dash = true;
    }
    if (length >= WS_MAX_PROJECT_NAME) {
      break;
    }
  }
  out[length] = '\0';
}

}  // namespace workspace_utils
