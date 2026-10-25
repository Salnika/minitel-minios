#ifndef STORAGE_WORKSPACE_UTILS_H
#define STORAGE_WORKSPACE_UTILS_H

#include <stddef.h>
#include <stdint.h>

#include <ArduinoJson.h>

#include "document_store.h"

// Project manager data served by Bibliotek (/projects, /tasks, /search). Display texts are
// transliterated to ASCII: the Minitel renderer only draws 7-bit characters.
constexpr size_t WS_MAX_PROJECT_NAME = 40;  // folder slug under projets/
constexpr size_t WS_MAX_PROJECT_TITLE = 32;
constexpr size_t WS_MAX_SHORT_TEXT = 40;  // one screen row
constexpr size_t WS_MAX_TASK_TEXT = 60;
constexpr size_t WS_DATE_LENGTH = 10;  // YYYY-MM-DD
constexpr size_t WS_TIME_LENGTH = 5;   // HH:MM

struct project_summary_t {
  char name[WS_MAX_PROJECT_NAME + 1];
  char title[WS_MAX_PROJECT_TITLE + 1];
  char status[WS_MAX_SHORT_TEXT + 1];
  char goal[WS_MAX_SHORT_TEXT + 1];
  char next[WS_MAX_SHORT_TEXT + 1];
  uint16_t open_tasks;
  uint16_t overdue_tasks;
  uint16_t week_minutes;
};

struct task_item_t {
  char file[STORE_MAX_DOCUMENT_ID + 1];  // server id: "projets/robot/taches.md"
  uint16_t line;
  char project[WS_MAX_PROJECT_NAME + 1];
  char text[WS_MAX_TASK_TEXT + 1];
  char due[WS_DATE_LENGTH + 1];
  char time[WS_TIME_LENGTH + 1];
  uint8_t priority;
  bool done;
};

struct search_hit_t {
  char id[STORE_MAX_DOCUMENT_ID + 1];
  uint16_t line;
  char snippet[WS_MAX_SHORT_TEXT + 1];
};

// A task as typed on the Minitel: "Commander colle !! @25/10 14h30".
struct task_input_t {
  char text[WS_MAX_TASK_TEXT + 1];
  char due[WS_DATE_LENGTH + 1];
  char time[WS_TIME_LENGTH + 1];
  uint8_t priority;
};

namespace workspace_utils {

int parse_projects(const JsonVariantConst& root, project_summary_t* out, int max_items);
bool parse_task(const JsonVariantConst& item, task_item_t* out);
int parse_tasks(const JsonVariantConst& root, task_item_t* out, int max_items, int* out_total);
int parse_search_hits(const JsonVariantConst& root, search_hit_t* out, int max_items);

// Reads "!".."!!!" priorities and a due date: "@25/10" (next occurrence), "@25/10/2027",
// "@2026-10-25", "@auj" or "@demain", optionally followed by a time "14:30" / "14h30" /
// "14h". `today` is YYYY-MM-DD. Fails when no text is left.
bool parse_task_input(const char* input, const char* today, task_input_t* out);

bool is_valid_date(const char* date);
bool add_days(const char* date, int days, char* out, size_t out_size);
// "auj.", "demain", "retard" or "25/10" for a task list column.
void format_due_label(const char* due, const char* today, char* out, size_t out_size);
// "Robot Minitel ete" -> "robot-minitel-ete", the server's folder rule.
void slugify(const char* input, char* out, size_t out_size);

}  // namespace workspace_utils

#endif
