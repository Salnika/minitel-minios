#include "tasks_screen.h"

#include <stdio.h>
#include <string.h>

#include "app/app_shell.h"
#include "app/lang.h"
#include "app/workspace.h"
#include "services/clock_service.h"
#include "storage/workspace_utils.h"
#include "ui/minitel_ui.h"

namespace {
constexpr uint8_t MAX_TASKS = 30;
constexpr uint8_t LIST_START_ROW = 5;
constexpr uint8_t LIST_ROWS = UI_STATUS_ROW - 1 - LIST_START_ROW;
constexpr uint8_t PAGE_STEP = 5;
static_assert(MAX_TASKS <= NET_MAX_TASKS, "net replies too small");

struct tasks_state_t {
  task_item_t tasks[MAX_TASKS];
  uint8_t count;
  int total;
  uint8_t selected;
  uint8_t scroll;
  bool all_open;  // false: due today or late
};

tasks_state_t g_tasks;

void ensure_visible() {
  if (g_tasks.selected < g_tasks.scroll) {
    g_tasks.scroll = g_tasks.selected;
  } else if (g_tasks.selected >= g_tasks.scroll + LIST_ROWS) {
    g_tasks.scroll = static_cast<uint8_t>(g_tasks.selected - LIST_ROWS + 1);
  }
}

void on_tasks_loaded(const net_request_t& request, const net_reply_t& reply) {
  if (!reply.ok) {
    shell::set_status(reply.error);
    return;
  }
  g_tasks.all_open = !request.params.tasks.due_today;
  g_tasks.total = reply.total;
  g_tasks.count = static_cast<uint8_t>(reply.count < MAX_TASKS ? reply.count : MAX_TASKS);
  memcpy(g_tasks.tasks, reply.data.tasks, sizeof(g_tasks.tasks[0]) * g_tasks.count);
  if (g_tasks.selected >= g_tasks.count) {
    g_tasks.selected = 0;
    g_tasks.scroll = 0;
  }
  ensure_visible();
  shell::clear_status();
  shell::request_redraw();
}

// Lists every open task, or those due today or late: the screen switches once they arrived.
void load_tasks(bool all_open) {
  net_request_t request =
      net::make(NET_LIST_TASKS, TR("Chargement des taches...", "Loading tasks..."), on_tasks_loaded);
  request.params.tasks.due_today = !all_open;
  request.params.tasks.max = MAX_TASKS;
  shell::clock_service().format_today(request.params.tasks.date, sizeof(request.params.tasks.date));
  if (!all_open && request.params.tasks.date[0] == '\0') {
    g_tasks.all_open = false;
    g_tasks.count = 0;
    g_tasks.total = 0;
    shell::set_status(TR("Heure non synchronisee: T pour tout", "Clock not synced: T for all"));
    return;
  }
  net::submit(request);
}

void on_task_toggled(const net_request_t& request, const net_reply_t& reply) {
  if (!reply.ok) {
    shell::set_status(reply.error);
    return;
  }
  const uint8_t index = static_cast<uint8_t>(request.context);
  if (index < g_tasks.count && g_tasks.tasks[index].line == request.params.toggle.line &&
      strcmp(g_tasks.tasks[index].file, request.params.toggle.file) == 0) {
    g_tasks.tasks[index].done = request.params.toggle.done;
  }
  workspace::request_refresh();
  shell::set_status(request.params.toggle.done ? TR("Tache faite", "Task done")
                                               : TR("Tache rouverte", "Task reopened"));
}

void toggle_selected() {
  if (g_tasks.selected >= g_tasks.count) {
    return;
  }
  const task_item_t& task = g_tasks.tasks[g_tasks.selected];
  const bool done = !task.done;
  net_request_t request = net::make(
      NET_SET_TASK_DONE, done ? TR("Tache faite...", "Task done...") : TR("Tache rouverte...", "Task reopened..."),
      on_task_toggled);
  strncpy(request.params.toggle.file, task.file, sizeof(request.params.toggle.file) - 1);
  request.params.toggle.line = task.line;
  request.params.toggle.done = done;
  request.context = g_tasks.selected;
  net::submit(request);
}

void open_selected_file() {
  if (g_tasks.selected >= g_tasks.count) {
    return;
  }
  const task_item_t& task = g_tasks.tasks[g_tasks.selected];
  char id[STORE_MAX_DOCUMENT_ID + 2];
  snprintf(id, sizeof(id), "/%s", task.file);
  shell::open_document_from(id, task.line, SCREEN_TASKS);
}

}  // namespace

void tasks_screen_open() {
  g_tasks.selected = 0;
  g_tasks.scroll = 0;
  shell::switch_screen(SCREEN_TASKS);
  load_tasks(g_tasks.all_open);
}

void tasks_screen_render() {
  shell::render_frame(TR("TACHES", "TASKS"),
                      g_tasks.all_open ? TR("ENVOI coche O ouvrir T aujourd'hui", "ENVOI tick O open T today")
                                       : TR("ENVOI coche O ouvrir T toutes", "ENVOI tick O open T all"));
  char line[UI_COLS + 1];
  snprintf(line, sizeof(line), "%s (%d)",
           g_tasks.all_open ? TR("Toutes les taches ouvertes", "All open tasks")
                            : TR("Aujourd'hui et en retard", "Today and late"),
           g_tasks.total);
  minitel_ui_write_line(3, line);
  if (g_tasks.count == 0) {
    minitel_ui_write_line(6, g_tasks.all_open ? TR("Rien a faire.", "Nothing to do.")
                                              : TR("Rien pour aujourd'hui.", "Nothing for today."));
  }

  char today[WS_DATE_LENGTH + 1];
  shell::clock_service().format_today(today, sizeof(today));
  for (uint8_t row = 0; row < LIST_ROWS && g_tasks.count > 0; ++row) {
    const uint8_t index = static_cast<uint8_t>(g_tasks.scroll + row);
    line[0] = '\0';
    if (index < g_tasks.count) {
      const task_item_t& task = g_tasks.tasks[index];
      char due[8];
      workspace_utils::format_due_label(task.due, today, due, sizeof(due));
      char text[WS_MAX_TASK_TEXT + 4];
      snprintf(text, sizeof(text), "%.*s%s%s", task.priority, "!!!", task.priority ? " " : "", task.text);
      snprintf(line, sizeof(line), "%c [%c] %-7.7s %-19.19s %6s", index == g_tasks.selected ? '>' : ' ',
               task.done ? 'x' : ' ', task.project, text, due);
    }
    minitel_ui_write_line(static_cast<uint8_t>(LIST_START_ROW + row), line);
  }
  shell::render_status_line();
}

void tasks_screen_handle(const input_event_t& event) {
  switch (event.type) {
    case INPUT_EVENT_UP:
      if (g_tasks.selected > 0) {
        --g_tasks.selected;
      }
      break;
    case INPUT_EVENT_DOWN:
      if (g_tasks.selected + 1 < g_tasks.count) {
        ++g_tasks.selected;
      }
      break;
    case INPUT_EVENT_PAGE_UP:
      g_tasks.selected = g_tasks.selected > PAGE_STEP ? static_cast<uint8_t>(g_tasks.selected - PAGE_STEP) : 0;
      break;
    case INPUT_EVENT_PAGE_DOWN:
      if (g_tasks.count > 0) {
        g_tasks.selected = static_cast<uint8_t>(
            g_tasks.selected + PAGE_STEP < g_tasks.count ? g_tasks.selected + PAGE_STEP : g_tasks.count - 1);
      }
      break;
    case INPUT_EVENT_ENTER:
      toggle_selected();
      return;
    case INPUT_EVENT_ESCAPE:
      shell::switch_screen(SCREEN_HOME);
      return;
    case INPUT_EVENT_PRINTABLE:
      if (event.ch == 't' || event.ch == 'T') {
        g_tasks.selected = 0;
        g_tasks.scroll = 0;
        load_tasks(!g_tasks.all_open);
      } else if (event.ch == 'o' || event.ch == 'O') {
        open_selected_file();
      } else if (event.ch == 'r' || event.ch == 'R') {
        load_tasks(g_tasks.all_open);
      }
      return;
    default:
      return;
  }
  ensure_visible();
  shell::request_redraw();
}

void tasks_screen_resume() {
  load_tasks(g_tasks.all_open);
}
