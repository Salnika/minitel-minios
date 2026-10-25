#include "projects_screen.h"

#include <stdio.h>
#include <string.h>

#include "app/app_shell.h"
#include "app/workspace.h"
#include "services/clock_service.h"
#include "storage/workspace_utils.h"
#include "ui/minitel_ui.h"

namespace {
constexpr uint8_t MAX_PROJECTS = 24;
constexpr uint8_t MAX_PROJECT_TASKS = 30;
constexpr uint8_t LIST_START_ROW = 5;
constexpr uint8_t LIST_ROWS = UI_STATUS_ROW - 1 - LIST_START_ROW;
constexpr uint8_t TASKS_START_ROW = 10;
constexpr uint8_t TASKS_ROWS = UI_STATUS_ROW - 1 - TASKS_START_ROW;
constexpr uint8_t PAGE_STEP = 5;
static_assert(MAX_PROJECTS <= NET_MAX_PROJECTS && MAX_PROJECT_TASKS <= NET_MAX_TASKS, "net replies too small");

struct projects_state_t {
  project_summary_t projects[MAX_PROJECTS];
  uint8_t count;
  uint8_t selected;
  uint8_t scroll;
  uint8_t current;  // project shown on SCREEN_PROJECT
  task_item_t tasks[MAX_PROJECT_TASKS];
  uint8_t task_count;
  uint8_t task_selected;
  uint8_t task_scroll;
  char open_after_load[WS_MAX_PROJECT_NAME + 1];  // project just created: open it once listed
  bool timer_switch;  // stopping another project's timer to start this one's
};

projects_state_t g_projects;

void today(char* out, size_t out_size) {
  shell::clock_service().format_today(out, out_size);
}

project_summary_t& current_project() {
  return g_projects.projects[g_projects.current];
}

// Keeps `selected` inside the window [scroll, scroll + rows).
void scroll_to(uint8_t selected, uint8_t rows, uint8_t* scroll) {
  if (selected < *scroll) {
    *scroll = selected;
  } else if (selected >= *scroll + rows) {
    *scroll = static_cast<uint8_t>(selected - rows + 1);
  }
}

// UP/DOWN/page keys on a list; returns true when the event was a move.
bool move_selection(const input_event_t& event, uint8_t count, uint8_t* selected) {
  if (count == 0) {
    return false;
  }
  switch (event.type) {
    case INPUT_EVENT_UP:
      if (*selected > 0) {
        --*selected;
      }
      return true;
    case INPUT_EVENT_DOWN:
      if (*selected + 1 < count) {
        ++*selected;
      }
      return true;
    case INPUT_EVENT_PAGE_UP:
      *selected = *selected > PAGE_STEP ? static_cast<uint8_t>(*selected - PAGE_STEP) : 0;
      return true;
    case INPUT_EVENT_PAGE_DOWN:
      *selected = static_cast<uint8_t>(*selected + PAGE_STEP < count ? *selected + PAGE_STEP : count - 1);
      return true;
    default:
      return false;
  }
}

void format_minutes(uint32_t minutes, char* out, size_t out_size) {
  snprintf(out, out_size, "%luh%02lu", static_cast<unsigned long>(minutes / 60),
           static_cast<unsigned long>(minutes % 60));
}

void load_project_tasks(const char* status);

void open_project(uint8_t index, const char* status) {
  if (index >= g_projects.count) {
    return;
  }
  g_projects.current = index;
  g_projects.task_selected = 0;
  g_projects.task_scroll = 0;
  g_projects.task_count = 0;
  shell::switch_screen(SCREEN_PROJECT);
  load_project_tasks(status);
}

void on_projects_loaded(const net_request_t& request, const net_reply_t& reply) {
  if (!reply.ok) {
    g_projects.open_after_load[0] = '\0';
    shell::set_status(reply.error);
    return;
  }
  g_projects.count = static_cast<uint8_t>(reply.count < MAX_PROJECTS ? reply.count : MAX_PROJECTS);
  memcpy(g_projects.projects, reply.data.projects, sizeof(g_projects.projects[0]) * g_projects.count);
  if (g_projects.selected >= g_projects.count) {
    g_projects.selected = g_projects.count > 0 ? static_cast<uint8_t>(g_projects.count - 1) : 0;
  }
  scroll_to(g_projects.selected, LIST_ROWS, &g_projects.scroll);

  if (g_projects.open_after_load[0] != '\0') {
    for (uint8_t i = 0; i < g_projects.count; ++i) {
      if (strcmp(g_projects.projects[i].name, g_projects.open_after_load) == 0) {
        g_projects.open_after_load[0] = '\0';
        g_projects.selected = i;
        open_project(i, "Projet cree: A pour une tache");
        return;
      }
    }
    g_projects.open_after_load[0] = '\0';
  }
  if (request.status) {
    shell::set_status(request.status);
  } else {
    shell::clear_status();
  }
  shell::request_redraw();
}

void load_projects() {
  net_request_t request = net::make(NET_LIST_PROJECTS, "Chargement des projets...", on_projects_loaded);
  today(request.params.projects.date, sizeof(request.params.projects.date));
  net::submit(request);
}

void on_project_tasks_loaded(const net_request_t& request, const net_reply_t& reply) {
  if (!reply.ok) {
    shell::set_status(reply.error);
    return;
  }
  if (strcmp(request.params.tasks.project, current_project().name) != 0) {
    return;  // another project is shown now
  }
  g_projects.task_count = static_cast<uint8_t>(reply.count < MAX_PROJECT_TASKS ? reply.count : MAX_PROJECT_TASKS);
  memcpy(g_projects.tasks, reply.data.tasks, sizeof(g_projects.tasks[0]) * g_projects.task_count);
  if (g_projects.task_selected >= g_projects.task_count) {
    g_projects.task_selected = 0;
    g_projects.task_scroll = 0;
  }
  if (request.status) {
    shell::set_status(request.status);
  } else {
    shell::clear_status();
  }
  shell::request_redraw();
}

// `status` (string literal) shows once the tasks arrived.
void load_project_tasks(const char* status) {
  net_request_t request = net::make(NET_LIST_TASKS, "Chargement des taches...", on_project_tasks_loaded);
  request.status = status;
  strncpy(request.params.tasks.project, current_project().name, sizeof(request.params.tasks.project) - 1);
  request.params.tasks.max = MAX_PROJECT_TASKS;
  today(request.params.tasks.date, sizeof(request.params.tasks.date));
  net::submit(request);
}

void on_project_created(const net_request_t& request, const net_reply_t& reply) {
  (void)request;
  if (!reply.ok) {
    shell::set_status(reply.error);
    return;
  }
  strncpy(g_projects.open_after_load, reply.data.project_name, sizeof(g_projects.open_after_load) - 1);
  g_projects.open_after_load[sizeof(g_projects.open_after_load) - 1] = '\0';
  load_projects();
}

void on_new_project(const char* title) {
  char name[WS_MAX_PROJECT_NAME + 1];
  workspace_utils::slugify(title, name, sizeof(name));
  if (name[0] == '\0') {
    shell::set_status("Nom vide: rien cree");
    return;
  }
  net_request_t request = net::make(NET_CREATE_PROJECT, "Creation du projet...", on_project_created);
  strncpy(request.params.project.title, title, sizeof(request.params.project.title) - 1);
  net::submit(request);
}

void on_task_added(const net_request_t& request, const net_reply_t& reply) {
  if (!reply.ok) {
    shell::set_status(reply.error);
    return;
  }
  if (strcmp(request.params.add_task.project, current_project().name) == 0) {
    ++current_project().open_tasks;
  }
  workspace::request_refresh();
  load_project_tasks(request.status);
}

void on_new_task(const char* value) {
  net_request_t request = net::make(NET_ADD_TASK, "Ajout de la tache...", on_task_added);
  char date[WS_DATE_LENGTH + 1];
  today(date, sizeof(date));
  if (!workspace_utils::parse_task_input(value, date, &request.params.add_task.task)) {
    shell::set_status("Tache vide: rien ajoute");
    return;
  }
  strncpy(request.params.add_task.project, current_project().name, sizeof(request.params.add_task.project) - 1);
  request.status = request.params.add_task.task.due[0] != '\0' ? "Tache ajoutee avec echeance" : "Tache ajoutee";
  net::submit(request);
}

void on_task_toggled(const net_request_t& request, const net_reply_t& reply) {
  if (!reply.ok) {
    shell::set_status(reply.error);
    return;
  }
  const bool done = request.params.toggle.done;
  const uint8_t index = static_cast<uint8_t>(request.context);
  if (index < g_projects.task_count && g_projects.tasks[index].line == request.params.toggle.line &&
      strcmp(g_projects.tasks[index].file, request.params.toggle.file) == 0) {
    g_projects.tasks[index].done = done;
  }
  project_summary_t& project = current_project();
  if (done && project.open_tasks > 0) {
    --project.open_tasks;
  } else if (!done) {
    ++project.open_tasks;
  }
  workspace::request_refresh();
  shell::set_status(done ? "Tache faite" : "Tache rouverte");
}

void toggle_selected_task() {
  if (g_projects.task_selected >= g_projects.task_count) {
    return;
  }
  const task_item_t& task = g_projects.tasks[g_projects.task_selected];
  const bool done = !task.done;
  net_request_t request = net::make(NET_SET_TASK_DONE, done ? "Tache faite..." : "Tache rouverte...", on_task_toggled);
  strncpy(request.params.toggle.file, task.file, sizeof(request.params.toggle.file) - 1);
  request.params.toggle.line = task.line;
  request.params.toggle.done = done;
  request.context = g_projects.task_selected;
  net::submit(request);
}

void on_timer_stopped(bool stopped, const char* status, uint32_t minutes) {
  if (!stopped) {
    shell::set_status(status);
    return;
  }
  project_summary_t& project = current_project();
  if (g_projects.timer_switch) {
    g_projects.timer_switch = false;
    if (!workspace::timer_start(project.name)) {
      shell::set_status("Chrono: heure pas encore synchronisee");
      return;
    }
    shell::set_status("Chrono lance: C pour l'arreter");
    return;
  }
  project.week_minutes = static_cast<uint16_t>(project.week_minutes + minutes);
  shell::set_status(status);
}

void toggle_timer() {
  project_summary_t& project = current_project();
  if (workspace::timer_running() && strcmp(workspace::timer_project(), project.name) == 0) {
    g_projects.timer_switch = false;
    workspace::timer_stop("Arret du chrono...", on_timer_stopped);
    return;
  }
  if (workspace::timer_running()) {
    // Switching project: the running timer is logged first.
    g_projects.timer_switch = true;
    workspace::timer_stop("Arret du chrono precedent...", on_timer_stopped);
    return;
  }
  if (!workspace::timer_start(project.name)) {
    shell::set_status("Chrono: heure pas encore synchronisee");
    return;
  }
  shell::set_status("Chrono lance: C pour l'arreter");
}

void open_project_file(const char* file_name) {
  char id[STORE_MAX_DOCUMENT_ID + 1];
  snprintf(id, sizeof(id), "/projets/%s/%s", current_project().name, file_name);
  shell::open_document_from(id, 0, SCREEN_PROJECT);
}

}  // namespace

void projects_screen_open() {
  shell::switch_screen(SCREEN_PROJECTS);
  load_projects();
}

void projects_screen_render() {
  shell::render_frame("PROJETS", "ENVOI ouvrir N nouveau R recharger");
  minitel_ui_write_line(3, "Taches a faire par projet (! retard)");
  if (g_projects.count == 0) {
    minitel_ui_write_line(6, "Aucun projet. N pour en creer un.");
    shell::render_status_line();
    return;
  }
  for (uint8_t row = 0; row < LIST_ROWS; ++row) {
    const uint8_t index = static_cast<uint8_t>(g_projects.scroll + row);
    char line[UI_COLS + 1] = "";
    if (index < g_projects.count) {
      const project_summary_t& project = g_projects.projects[index];
      char overdue[8] = "";
      if (project.overdue_tasks > 0) {
        snprintf(overdue, sizeof(overdue), " !%u", project.overdue_tasks);
      }
      snprintf(line, sizeof(line), "%c %-28.28s %3u%s", index == g_projects.selected ? '>' : ' ',
               project.title, project.open_tasks, overdue);
    }
    minitel_ui_write_line(static_cast<uint8_t>(LIST_START_ROW + row), line);
  }
  shell::render_status_line();
}

void projects_screen_handle(const input_event_t& event) {
  if (move_selection(event, g_projects.count, &g_projects.selected)) {
    scroll_to(g_projects.selected, LIST_ROWS, &g_projects.scroll);
    shell::request_redraw();
    return;
  }
  switch (event.type) {
    case INPUT_EVENT_ENTER:
      open_project(g_projects.selected, nullptr);
      return;
    case INPUT_EVENT_ESCAPE:
      shell::switch_screen(SCREEN_HOME);
      return;
    case INPUT_EVENT_PRINTABLE:
      if (event.ch == 'n' || event.ch == 'N') {
        shell::request_text_input(SCREEN_PROJECTS, "Nouveau projet", "Nom du projet", "", false,
                                  on_new_project);
      } else if (event.ch == 'r' || event.ch == 'R') {
        load_projects();
      }
      return;
    default:
      return;
  }
}

void project_screen_render() {
  const project_summary_t& project = current_project();
  shell::render_frame("PROJET", "ENVOI coche A ajout F fiche T taches");
  char line[UI_COLS + 1];

  if (workspace::timer_running() && strcmp(workspace::timer_project(), project.name) == 0) {
    char elapsed[12];
    format_minutes(workspace::timer_elapsed_minutes(), elapsed, sizeof(elapsed));
    snprintf(line, sizeof(line), "%.28s  [%s]", project.title, elapsed);
  } else {
    snprintf(line, sizeof(line), "%.40s", project.title);
  }
  minitel_ui_write_line(3, line);
  snprintf(line, sizeof(line), "Statut: %.32s", project.status[0] ? project.status : "-");
  minitel_ui_write_line(4, line);
  snprintf(line, sizeof(line), "Objectif: %.30s", project.goal[0] ? project.goal : "-");
  minitel_ui_write_line(5, line);
  snprintf(line, sizeof(line), "Suite: %.33s", project.next[0] ? project.next : "-");
  minitel_ui_write_line(6, line);
  char week[12];
  format_minutes(project.week_minutes, week, sizeof(week));
  snprintf(line, sizeof(line), "Semaine: %s  A faire: %u (%u retard)", week, project.open_tasks,
           project.overdue_tasks);
  minitel_ui_write_line(7, line);
  minitel_ui_write_line(8, "C chrono  R recharger  RETOUR projets");
  minitel_ui_write_line(9, g_projects.task_count > 0 ? "Taches ouvertes:" : "Aucune tache ouverte. A pour ajouter.");

  char date[WS_DATE_LENGTH + 1];
  today(date, sizeof(date));
  for (uint8_t row = 0; row < TASKS_ROWS; ++row) {
    const uint8_t index = static_cast<uint8_t>(g_projects.task_scroll + row);
    line[0] = '\0';
    if (index < g_projects.task_count) {
      const task_item_t& task = g_projects.tasks[index];
      char due[8];
      workspace_utils::format_due_label(task.due, date, due, sizeof(due));
      char text[WS_MAX_TASK_TEXT + 4];
      snprintf(text, sizeof(text), "%.*s%s%s", task.priority, "!!!", task.priority ? " " : "", task.text);
      snprintf(line, sizeof(line), "%c [%c] %-25.25s %6s", index == g_projects.task_selected ? '>' : ' ',
               task.done ? 'x' : ' ', text, due);
    }
    minitel_ui_write_line(static_cast<uint8_t>(TASKS_START_ROW + row), line);
  }
  shell::render_status_line();
}

void project_screen_handle(const input_event_t& event) {
  if (move_selection(event, g_projects.task_count, &g_projects.task_selected)) {
    scroll_to(g_projects.task_selected, TASKS_ROWS, &g_projects.task_scroll);
    shell::request_redraw();
    return;
  }
  switch (event.type) {
    case INPUT_EVENT_ENTER:
      toggle_selected_task();
      return;
    case INPUT_EVENT_ESCAPE:
      shell::switch_screen(SCREEN_PROJECTS);
      return;
    case INPUT_EVENT_PRINTABLE:
      switch (event.ch) {
        case 'a':
        case 'A':
          shell::request_text_input(SCREEN_PROJECT, "Nouvelle tache", "Texte !! @25/10 14h30", "", false,
                                    on_new_task);
          return;
        case 'f':
        case 'F':
          open_project_file("projet.md");
          return;
        case 't':
        case 'T':
          open_project_file("taches.md");
          return;
        case 'c':
        case 'C':
          toggle_timer();
          return;
        case 'r':
        case 'R':
          load_project_tasks(nullptr);
          return;
        default:
          return;
      }
    default:
      return;
  }
}

void project_screen_resume() {
  load_project_tasks(nullptr);
}
