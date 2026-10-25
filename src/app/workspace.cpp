#include "workspace.h"

#include <Arduino.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "app/app_shell.h"
#include "app/lang.h"
#include "platform/app_log.h"
#include "services/clock_service.h"
#include "services/settings_service.h"
#include "services/wifi_service.h"
#include "storage/api_document_store.h"
#include "ui/minitel_ui.h"

namespace workspace {
namespace {

constexpr uint32_t TODAY_REFRESH_MS = 5UL * 60UL * 1000UL;
constexpr uint32_t REMINDER_CHECK_MS = 15000;
constexpr uint8_t TODAY_MAX_TASKS = 24;
constexpr uint8_t REMINDED_MAX = 24;
constexpr const char* KEY_TIMER_PROJECT = "timer_proj";
constexpr const char* KEY_TIMER_START = "timer_start";
constexpr const char* KEY_RECENTS = "recents";

struct today_state_t {
  bool known;
  bool refresh_requested;
  uint32_t last_refresh_ms;
  char date[WS_DATE_LENGTH + 1];
  task_item_t tasks[TODAY_MAX_TASKS];
  uint8_t task_count;
  uint16_t open_count;
  uint16_t overdue_count;
  uint32_t reminded[REMINDED_MAX];  // hashes of the tasks already rung today
  uint8_t reminded_count;
  uint32_t last_reminder_check_ms;
};

struct timer_state_t {
  char project[WS_MAX_PROJECT_NAME + 1];
  uint32_t start_epoch;
  timer_stopped_t stop_done;  // caller waiting for the time to be logged
};

today_state_t g_today;
timer_state_t g_timer;
recent_document_t g_recents[RECENT_MAX];
uint8_t g_recent_count = 0;
const recent_document_t g_no_recent = {};

void safe_copy(char* dest, size_t dest_size, const char* src) {
  if (!dest || dest_size == 0) {
    return;
  }
  strncpy(dest, src ? src : "", dest_size - 1);
  dest[dest_size - 1] = '\0';
}

uint32_t task_hash(const task_item_t& task) {
  uint32_t hash = 2166136261u;  // FNV-1a
  for (const char* cursor = task.file; *cursor != '\0'; ++cursor) {
    hash = (hash ^ static_cast<uint8_t>(*cursor)) * 16777619u;
  }
  return (hash ^ task.line) * 16777619u;
}

bool already_reminded(uint32_t hash) {
  for (uint8_t i = 0; i < g_today.reminded_count; ++i) {
    if (g_today.reminded[i] == hash) {
      return true;
    }
  }
  return false;
}

void on_today_loaded(const net_request_t& request, const net_reply_t& reply) {
  if (!reply.ok) {
    return;  // keep the last known numbers
  }
  const char* today = request.params.tasks.date;
  if (strcmp(today, g_today.date) != 0) {
    safe_copy(g_today.date, sizeof(g_today.date), today);
    g_today.reminded_count = 0;  // a new day: every reminder can ring again
  }
  g_today.task_count = static_cast<uint8_t>(reply.count < TODAY_MAX_TASKS ? reply.count : TODAY_MAX_TASKS);
  memcpy(g_today.tasks, reply.data.tasks, sizeof(g_today.tasks[0]) * g_today.task_count);
  g_today.open_count = static_cast<uint16_t>(reply.total);
  g_today.overdue_count = 0;
  for (uint8_t i = 0; i < g_today.task_count; ++i) {
    if (strcmp(g_today.tasks[i].due, today) < 0) {
      ++g_today.overdue_count;
    }
  }
  g_today.known = true;
  if (shell::current_screen() == SCREEN_HOME) {
    shell::request_redraw();  // the dashboard shows these counts
  }
  app_log_info("workspace", "today tasks=%u overdue=%u", g_today.open_count, g_today.overdue_count);
}

// In the background: the loop goes on drawing and reading keys meanwhile.
void refresh_today() {
  g_today.last_refresh_ms = millis();
  g_today.refresh_requested = false;
  net_request_t request = net::make(NET_LIST_TASKS, nullptr, on_today_loaded);
  request.background = true;
  request.params.tasks.due_today = true;
  request.params.tasks.max = TODAY_MAX_TASKS;
  shell::clock_service().format_today(request.params.tasks.date, sizeof(request.params.tasks.date));
  if (request.params.tasks.date[0] == '\0' || !shell::wifi().is_connected()) {
    return;
  }
  net::submit(request);
}

void check_reminders() {
  struct tm now;
  if (!g_today.known || !shell::clock_service().local_time(&now)) {
    return;
  }
  char today[WS_DATE_LENGTH + 1];
  char now_hhmm[WS_TIME_LENGTH + 1];
  strftime(today, sizeof(today), "%Y-%m-%d", &now);
  strftime(now_hhmm, sizeof(now_hhmm), "%H:%M", &now);
  if (strcmp(today, g_today.date) != 0) {
    return;  // yesterday's list: wait for the next refresh
  }

  for (uint8_t i = 0; i < g_today.task_count; ++i) {
    const task_item_t& task = g_today.tasks[i];
    if (task.done || task.time[0] == '\0' || strcmp(task.due, today) != 0 ||
        strcmp(task.time, now_hhmm) > 0) {
      continue;
    }
    const uint32_t hash = task_hash(task);
    if (already_reminded(hash)) {
      continue;
    }
    if (g_today.reminded_count < REMINDED_MAX) {
      g_today.reminded[g_today.reminded_count++] = hash;
    }
    char status[UI_COLS + 1];
    snprintf(status, sizeof(status), TR("Rappel %s: %.27s", "Reminder %s: %.25s"), task.time, task.text);
    shell::set_status(status);
    minitel_ui_beep(3, 200);
    return;  // one reminder at a time, the next check rings the following one
  }
}

void save_timer() {
  char start[16];
  snprintf(start, sizeof(start), "%lu", static_cast<unsigned long>(g_timer.start_epoch));
  shell::settings_service().store_value(KEY_TIMER_PROJECT, g_timer.project);
  shell::settings_service().store_value(KEY_TIMER_START, g_timer.project[0] != '\0' ? start : "");
}

void clear_timer() {
  g_timer.project[0] = '\0';
  g_timer.start_epoch = 0;
  save_timer();
}

void on_timer_logged(const net_request_t& request, const net_reply_t& reply) {
  const timer_stopped_t done = g_timer.stop_done;
  g_timer.stop_done = nullptr;
  if (!reply.ok) {
    if (done) {
      // The timer keeps running.
      done(false, reply.error[0] ? reply.error : TR("Chrono non note", "Timer not logged"), 0);
    }
    return;
  }
  clear_timer();
  char status[UI_COLS + 1];
  snprintf(status, sizeof(status), TR("Chrono: %lu min notees", "Timer: %lu min logged"),
           static_cast<unsigned long>(request.context));
  if (done) {
    done(true, status, request.context);
  }
}

void on_timer_log_cancelled(const net_request_t& request) {
  (void)request;
  g_timer.stop_done = nullptr;  // nothing logged: the timer keeps running
}

void save_recents() {
  char buffer[RECENT_MAX * (sizeof(recent_document_t) + 2)];
  size_t length = 0;
  buffer[0] = '\0';
  for (uint8_t i = 0; i < g_recent_count; ++i) {
    const int written = snprintf(buffer + length, sizeof(buffer) - length, "%s\t%s\n", g_recents[i].id,
                                 g_recents[i].title);
    if (written < 0 || static_cast<size_t>(written) >= sizeof(buffer) - length) {
      break;
    }
    length += static_cast<size_t>(written);
  }
  shell::settings_service().store_value(KEY_RECENTS, buffer);
}

void load_recents() {
  char buffer[RECENT_MAX * (sizeof(recent_document_t) + 2)];
  shell::settings_service().load_value(KEY_RECENTS, buffer, sizeof(buffer));
  g_recent_count = 0;
  char* saveptr = nullptr;
  for (char* line = strtok_r(buffer, "\n", &saveptr); line && g_recent_count < RECENT_MAX;
       line = strtok_r(nullptr, "\n", &saveptr)) {
    char* tab = strchr(line, '\t');
    if (!tab) {
      continue;
    }
    *tab = '\0';
    safe_copy(g_recents[g_recent_count].id, sizeof(g_recents[0].id), line);
    safe_copy(g_recents[g_recent_count].title, sizeof(g_recents[0].title), tab + 1);
    ++g_recent_count;
  }
}

}  // namespace

void begin() {
  memset(&g_today, 0, sizeof(g_today));
  memset(&g_timer, 0, sizeof(g_timer));
  shell::settings_service().load_value(KEY_TIMER_PROJECT, g_timer.project, sizeof(g_timer.project));
  char start[16];
  shell::settings_service().load_value(KEY_TIMER_START, start, sizeof(start));
  g_timer.start_epoch = static_cast<uint32_t>(strtoul(start, nullptr, 10));
  if (g_timer.start_epoch == 0) {
    g_timer.project[0] = '\0';
  }
  load_recents();
}

void tick() {
  const uint32_t now = millis();
  const bool due = g_today.refresh_requested || g_today.last_refresh_ms == 0 ||
                   now - g_today.last_refresh_ms >= TODAY_REFRESH_MS;
  // A refresh asked while one runs waits for it: it may miss the latest change.
  if (due && shell::clock_service().is_synced() && !net::background_pending(NET_LIST_TASKS)) {
    refresh_today();
  }
  if (now - g_today.last_reminder_check_ms >= REMINDER_CHECK_MS) {
    g_today.last_reminder_check_ms = now;
    check_reminders();
  }
}

void request_refresh() {
  g_today.refresh_requested = true;
}

bool today_known() {
  return g_today.known;
}

uint16_t today_open_count() {
  return g_today.open_count;
}

uint16_t today_overdue_count() {
  return g_today.overdue_count;
}

bool timer_running() {
  return g_timer.project[0] != '\0';
}

const char* timer_project() {
  return g_timer.project;
}

uint32_t timer_elapsed_minutes() {
  const time_t now = time(nullptr);
  if (!timer_running() || !shell::clock_service().is_synced() ||
      now < static_cast<time_t>(g_timer.start_epoch)) {
    return 0;
  }
  return static_cast<uint32_t>((now - static_cast<time_t>(g_timer.start_epoch)) / 60);
}

bool timer_start(const char* project) {
  if (!project || project[0] == '\0' || !shell::clock_service().is_synced()) {
    return false;
  }
  safe_copy(g_timer.project, sizeof(g_timer.project), project);
  g_timer.start_epoch = static_cast<uint32_t>(time(nullptr));
  save_timer();
  return true;
}

bool timer_stop(const char* label, timer_stopped_t done) {
  if (!timer_running()) {
    if (done) {
      done(false, TR("Aucun chrono en cours", "No timer running"), 0);
    }
    return true;
  }

  const uint32_t minutes = timer_elapsed_minutes();
  if (minutes < 1) {
    clear_timer();
    if (done) {
      done(true, TR("Chrono arrete (moins d'1 min)", "Timer stopped (under 1 min)"), 0);
    }
    return true;
  }

  const time_t start = static_cast<time_t>(g_timer.start_epoch);
  struct tm start_tm;
  localtime_r(&start, &start_tm);
  char started_at[20];
  strftime(started_at, sizeof(started_at), "%Y-%m-%d %H:%M", &start_tm);
  net_request_t request = net::make(NET_APPEND_LINE, label, on_timer_logged);
  request.on_cancelled = on_timer_log_cancelled;
  request.context = minutes;
  snprintf(request.params.append.id, sizeof(request.params.append.id), "projets/%s/temps.md", g_timer.project);
  snprintf(request.params.append.text, sizeof(request.params.append.text), "- %s +%lu min", started_at,
           static_cast<unsigned long>(minutes));
  if (!net::submit(request)) {
    return false;
  }
  g_timer.stop_done = done;
  return true;
}

void recent_add(const char* id, const char* title) {
  if (!id || id[0] == '\0') {
    return;
  }
  uint8_t existing = g_recent_count;
  for (uint8_t i = 0; i < g_recent_count; ++i) {
    if (strcmp(g_recents[i].id, id) == 0) {
      existing = i;
      break;
    }
  }
  const uint8_t last = existing < g_recent_count ? existing
                                                 : (g_recent_count < RECENT_MAX ? g_recent_count : RECENT_MAX - 1);
  for (uint8_t i = last; i > 0; --i) {
    g_recents[i] = g_recents[i - 1];
  }
  safe_copy(g_recents[0].id, sizeof(g_recents[0].id), id);
  safe_copy(g_recents[0].title, sizeof(g_recents[0].title), title ? title : id);
  if (existing >= g_recent_count && g_recent_count < RECENT_MAX) {
    ++g_recent_count;
  }
  save_recents();
}

uint8_t recent_count() {
  return g_recent_count;
}

const recent_document_t& recent_at(uint8_t index) {
  return index < g_recent_count ? g_recents[index] : g_no_recent;
}

}  // namespace workspace
