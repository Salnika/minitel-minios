#include "search_screen.h"

#include <stdio.h>
#include <string.h>

#include "app/app_shell.h"
#include "app/lang.h"
#include "storage/workspace_utils.h"
#include "ui/minitel_ui.h"

namespace {
constexpr uint8_t MAX_HITS = 20;
constexpr uint8_t LIST_START_ROW = 5;
// Each hit takes two rows: where it is, then the matching text.
constexpr uint8_t VISIBLE_HITS = (UI_STATUS_ROW - 1 - LIST_START_ROW) / 2;
static_assert(MAX_HITS <= NET_MAX_HITS, "net replies too small");

struct search_state_t {
  char query[UI_COLS + 1];
  search_hit_t hits[MAX_HITS];
  uint8_t count;
  uint8_t selected;
  uint8_t scroll;
};

search_state_t g_search;

void ensure_visible() {
  if (g_search.selected < g_search.scroll) {
    g_search.scroll = g_search.selected;
  } else if (g_search.selected >= g_search.scroll + VISIBLE_HITS) {
    g_search.scroll = static_cast<uint8_t>(g_search.selected - VISIBLE_HITS + 1);
  }
}

void on_search_done(const net_request_t& request, const net_reply_t& reply) {
  (void)request;
  if (!reply.ok) {
    shell::set_status(reply.error);
    return;
  }
  const int count = reply.count < MAX_HITS ? reply.count : MAX_HITS;
  memcpy(g_search.hits, reply.data.hits, sizeof(g_search.hits[0]) * count);
  g_search.count = static_cast<uint8_t>(count);
  shell::request_redraw();
  if (count == 0) {
    shell::set_status(TR("Aucun resultat", "No result"));
    return;
  }
  char status[UI_COLS + 1];
  snprintf(status, sizeof(status), TR("%d resultat(s)", "%d result(s)"), count);
  shell::set_status(status);
}

void on_query(const char* query) {
  strncpy(g_search.query, query ? query : "", sizeof(g_search.query) - 1);
  g_search.query[sizeof(g_search.query) - 1] = '\0';
  if (g_search.query[0] == '\0') {
    shell::set_status(TR("Recherche vide", "Empty search"));
    return;
  }

  g_search.count = 0;
  g_search.selected = 0;
  g_search.scroll = 0;
  shell::switch_screen(SCREEN_SEARCH);
  net_request_t request = net::make(NET_SEARCH, TR("Recherche...", "Searching..."), on_search_done);
  strncpy(request.params.search.query, g_search.query, sizeof(request.params.search.query) - 1);
  net::submit(request);
}

void ask_query() {
  shell::request_text_input(SCREEN_HOME, TR("Recherche", "Search"),
                            TR("Dans les notes et les projets", "In notes and projects"), g_search.query, false,
                            on_query);
}

}  // namespace

void search_screen_start() {
  ask_query();
}

void search_screen_render() {
  shell::render_frame(TR("RECHERCHE", "SEARCH"), TR("ENVOI ouvrir N nouvelle RETOUR", "ENVOI open N new RETOUR"));
  char line[UI_COLS + 1];
  snprintf(line, sizeof(line), "\"%.36s\"", g_search.query);
  minitel_ui_write_line(3, line);

  for (uint8_t slot = 0; slot < VISIBLE_HITS; ++slot) {
    const uint8_t index = static_cast<uint8_t>(g_search.scroll + slot);
    const uint8_t row = static_cast<uint8_t>(LIST_START_ROW + slot * 2);
    if (index >= g_search.count) {
      minitel_ui_write_line(row, "");
      minitel_ui_write_line(static_cast<uint8_t>(row + 1), "");
      continue;
    }
    const search_hit_t& hit = g_search.hits[index];
    snprintf(line, sizeof(line), "%c %.30s l.%u", index == g_search.selected ? '>' : ' ', hit.id, hit.line);
    minitel_ui_write_line(row, line);
    snprintf(line, sizeof(line), "    %.36s", hit.snippet);
    minitel_ui_write_line(static_cast<uint8_t>(row + 1), line);
  }
  shell::render_status_line();
}

void search_screen_handle(const input_event_t& event) {
  switch (event.type) {
    case INPUT_EVENT_UP:
      if (g_search.selected > 0) {
        --g_search.selected;
      }
      break;
    case INPUT_EVENT_DOWN:
      if (g_search.selected + 1 < g_search.count) {
        ++g_search.selected;
      }
      break;
    case INPUT_EVENT_ENTER:
      if (g_search.selected < g_search.count) {
        char id[STORE_MAX_DOCUMENT_ID + 2];
        snprintf(id, sizeof(id), "/%s", g_search.hits[g_search.selected].id);
        shell::open_document_from(id, g_search.hits[g_search.selected].line, SCREEN_SEARCH);
      }
      return;
    case INPUT_EVENT_ESCAPE:
      shell::switch_screen(SCREEN_HOME);
      return;
    case INPUT_EVENT_PRINTABLE:
      if (event.ch == 'n' || event.ch == 'N') {
        ask_query();
      }
      return;
    default:
      return;
  }
  ensure_visible();
  shell::request_redraw();
}
