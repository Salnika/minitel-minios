#include "app.h"

#include <Arduino.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_shell.h"
#include "editor_layout.h"
#include "input.h"
#include "lang.h"
#include "net_requests.h"
#include "version.h"
#include "workspace.h"
#include "screens/lock_screen.h"
#include "screens/model_picker.h"
#include "screens/projects_screen.h"
#include "screens/search_screen.h"
#include "screens/system_screen.h"
#include "screens/tasks_screen.h"
#include "text_wrap.h"
#include "../services/clock_service.h"
#include "../services/ota_service.h"
#include "../services/settings_service.h"
#include "../services/tls_roots.h"
#include "../services/wifi_service.h"
#include "../ui/minitel_link.h"
#include "../ui/minitel_ui.h"
#include "../platform/app_log.h"
#include "../platform/arduino_wrapper.h"

// Speed asked of the Minitel once found (a 1B goes up to 4800). 1200 keeps its power-on speed.
#ifndef MINITEL_LINK_BAUD
#define MINITEL_LINK_BAUD 4800
#endif

namespace {
constexpr int8_t MINITEL_RX_PIN = 22;
constexpr int8_t MINITEL_TX_PIN = 23;
constexpr uint32_t MINITEL_POWER_ON_BAUD = 1200;
constexpr uint32_t TERMINAL_WAKE_SEQUENCE_STEP_MS = 900;
constexpr uint8_t BOOT_LOGO_ROW = 4;
constexpr uint8_t BOOT_LOGO_COL = 3;
constexpr uint8_t BOOT_LOGO_WIDTH = 36;
constexpr uint8_t BOOT_LOGO_HEIGHT = 3;
constexpr uint8_t BOOT_STATUS_LINE1_ROW = UI_CONTENT_END_ROW - 1;
constexpr uint8_t BOOT_STATUS_LINE2_ROW = UI_CONTENT_END_ROW;
constexpr uint8_t MAX_EXPLORER_ITEMS = 16;
constexpr uint8_t HOME_MENU_COUNT = 9;
constexpr uint8_t HOME_MENU_START_ROW = 7;
constexpr uint8_t HOME_RECENTS_ROW = 17;
constexpr const char* HOME_MENU_LABELS[HOME_MENU_COUNT] = {
    TR("Projets", "Projects"),
    TR("Taches du jour", "Today's tasks"),
    TR("Notes", "Notes"),
    TR("Capture rapide", "Quick capture"),
    TR("Journal du jour", "Today's journal"),
    TR("Recherche", "Search"),
    TR("Assistant IA", "AI assistant"),
    TR("Reglages", "Settings"),
    TR("Systeme", "System")};
constexpr const char* DAY_NAMES[] = {
    TR("Dimanche", "Sunday"), TR("Lundi", "Monday"), TR("Mardi", "Tuesday"),
    TR("Mercredi", "Wednesday"), TR("Jeudi", "Thursday"), TR("Vendredi", "Friday"),
    TR("Samedi", "Saturday")};
constexpr const char* MONTH_NAMES[] = {
    TR("janvier", "January"), TR("fevrier", "February"), TR("mars", "March"),
    TR("avril", "April"), TR("mai", "May"), TR("juin", "June"),
    TR("juillet", "July"), TR("aout", "August"), TR("septembre", "September"),
    TR("octobre", "October"), TR("novembre", "November"), TR("decembre", "December")};
constexpr const char* INBOX_FILE = "inbox/inbox.md";
constexpr const char* OTA_HOSTNAME = "minitel-minios";
constexpr uint8_t SETTINGS_ITEM_COUNT = 10;
constexpr uint8_t SETTINGS_AI_TOOLS_INDEX = 6;
constexpr uint8_t SETTINGS_TIMEZONE_INDEX = 7;
constexpr uint8_t SETTINGS_PIN_INDEX = 8;
constexpr uint8_t SETTINGS_RESET_INDEX = SETTINGS_ITEM_COUNT - 1;
constexpr uint8_t STATUS_ROW = UI_STATUS_ROW;
constexpr uint8_t FILES_LIST_START_ROW = 6;
constexpr uint8_t FILES_META_ROW = STATUS_ROW - 2;
constexpr uint8_t FILES_HELP_ROW = STATUS_ROW - 1;
constexpr uint8_t FILES_VISIBLE_ROWS = FILES_META_ROW - FILES_LIST_START_ROW;
constexpr uint8_t WIFI_LIST_START_ROW = 7;
constexpr uint8_t WIFI_INFO_ROW = STATUS_ROW - 2;
constexpr uint8_t WIFI_VISIBLE_ROWS = WIFI_INFO_ROW - WIFI_LIST_START_ROW;
constexpr uint8_t TEXT_INPUT_BUFFER_ROW = 6;
constexpr uint8_t TEXT_INPUT_VISIBLE_COLUMNS = UI_COLS;
constexpr uint8_t CHAT_MODEL_ROW = 3;
constexpr uint8_t CHAT_TRANSCRIPT_START_ROW = 4;
// Agent mode lines: an action the assistant ran, then the one it asks the user about.
constexpr const char* CHAT_ACTION_PREFIX = " * ";
constexpr const char* CHAT_CONFIRM_PREFIX = " ? ";
constexpr const char* CHAT_CONTINUATION_PREFIX = "   ";
// Kept when the assistant acted but wrote nothing (it failed, or was stopped).
constexpr const char* CHAT_NO_REPLY = TR("(pas de reponse)", "(no reply)");
constexpr uint8_t CHAT_VISIBLE_ROWS = UI_CONTENT_END_ROW - CHAT_TRANSCRIPT_START_ROW + 1;
constexpr uint8_t CHAT_MAX_RENDER_LINES = 96;
// A streamed reply that reaches the bottom row jumps a page, keeping this many lines above
// the newest one: scrolling line by line would resend the whole transcript to the Minitel.
constexpr uint8_t CHAT_PAGE_CONTEXT_LINES = 2;
constexpr uint8_t EDITOR_BODY_START_ROW = 4;
constexpr uint8_t EDITOR_BODY_END_ROW = STATUS_ROW - 1;
constexpr uint8_t EDITOR_VISIBLE_LINES = EDITOR_BODY_END_ROW - EDITOR_BODY_START_ROW + 1;
constexpr uint8_t EDITOR_VISIBLE_COLUMNS = UI_COLS;
constexpr uint8_t EDITOR_MENU_START_ROW = 15;
constexpr uint8_t EDITOR_LAST_SEARCH_SIZE = 33;
const uint8_t BOOT_LOGO_MASKS[BOOT_LOGO_HEIGHT][BOOT_LOGO_WIDTH] = {
  { 0x3F, 0x3F, 0x3C, 0x00, 0x00, 0x3C, 0x3F, 0x3F, 0x00, 0x3C, 0x3C, 0x00, 0x3C, 0x3C, 0x00, 0x00, 0x3C, 0x3C, 0x00, 0x3C, 0x3C, 0x00, 0x3C, 0x3F, 0x3F, 0x3F, 0x3F, 0x3C, 0x00, 0x3C, 0x3F, 0x3F, 0x3F, 0x3F, 0x3F, 0x00 },
  { 0x3F, 0x3F, 0x00, 0x0F, 0x0F, 0x00, 0x3F, 0x3F, 0x00, 0x3F, 0x3F, 0x00, 0x3F, 0x3F, 0x3F, 0x3C, 0x3F, 0x3F, 0x00, 0x3F, 0x3F, 0x00, 0x3F, 0x3F, 0x00, 0x00, 0x3F, 0x3F, 0x00, 0x0F, 0x0F, 0x0F, 0x3C, 0x3C, 0x3C, 0x00 },
  { 0x3F, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x3F, 0x3F, 0x00, 0x3F, 0x3F, 0x00, 0x3F, 0x3F, 0x00, 0x0F, 0x3F, 0x3F, 0x00, 0x3F, 0x3F, 0x00, 0x0F, 0x3F, 0x3F, 0x3F, 0x3F, 0x0F, 0x00, 0x3F, 0x3F, 0x3F, 0x3F, 0x3F, 0x0F, 0x00 },
};

enum text_input_target_t {
  TEXT_INPUT_NONE = 0,
  TEXT_INPUT_WIFI_PASSWORD,
  TEXT_INPUT_API_URL,
  TEXT_INPUT_API_TOKEN,
  TEXT_INPUT_AI_BASE_URL,
  TEXT_INPUT_AI_API_KEY,
  TEXT_INPUT_TIMEZONE,
  TEXT_INPUT_NEW_DOCUMENT,
  TEXT_INPUT_NEW_FOLDER,
  TEXT_INPUT_EDIT_TAGS,
  TEXT_INPUT_FILTER_TAG,
  TEXT_INPUT_SEARCH_QUERY,
  TEXT_INPUT_GOTO_LINE,
  TEXT_INPUT_CHAT_PROMPT,
  TEXT_INPUT_CALLBACK
};

enum editor_overlay_t {
  EDITOR_OVERLAY_NONE = 0,
  EDITOR_OVERLAY_COMMAND_MENU,
  EDITOR_OVERLAY_CONFIRM_EXIT
};

enum editor_command_t {
  EDITOR_COMMAND_SEARCH = 0,
  EDITOR_COMMAND_GOTO_LINE,
  EDITOR_COMMAND_QUIT,
  EDITOR_COMMAND_REDRAW,
  EDITOR_COMMAND_COUNT
};

enum explorer_item_kind_t {
  EXPLORER_ITEM_FOLDER = 0,
  EXPLORER_ITEM_DOCUMENT
};

struct explorer_item_t {
  explorer_item_kind_t kind;
  char label[STORE_MAX_TITLE_LENGTH + STORE_MAX_EXTENSION_LENGTH + 4];
  char path[STORE_MAX_FOLDER_PATH + 1];
  char document_id[STORE_MAX_DOCUMENT_ID + 1];
  char meta[STORE_MAX_TAGS_SUMMARY_LENGTH + 1];  // full tag list: it pre-fills tag editing
};

struct explorer_state_t {
  char current_folder[STORE_MAX_FOLDER_PATH + 1];
  char tag_filter[STORE_MAX_TAG_LENGTH + 1];
  explorer_item_t items[MAX_EXPLORER_ITEMS];
  uint8_t item_count;
  uint8_t selected_index;
  uint8_t scroll_offset;
  bool stale;  // a document was created from here: list again when coming back
};

struct wifi_state_t {
  lm_wifi_network_t networks[NET_MAX_NETWORKS];
  uint8_t network_count;
  uint8_t selected_index;
  uint8_t scroll_offset;
  char pending_ssid[SETTINGS_MAX_WIFI_SSID + 1];
};

struct settings_state_t {
  uint8_t selected_index;
  bool reset_armed;  // first ENVOI on "Reset settings" only asks for confirmation
};

struct text_input_state_t {
  text_input_target_t target;
  screen_id_t return_screen;
  bool masked;
  char title[24];
  char prompt[41];
  char buffer[CHAT_MAX_PROMPT_LENGTH + 1];
  uint16_t length;
  uint16_t cursor;
  text_input_done_t on_done;  // TEXT_INPUT_CALLBACK only
};

struct editor_state_t {
  bool active;
  bool dirty;
  uint32_t revision;  // counts edits: a save only clears `dirty` if none came after it
  document_record_t document;
  uint16_t cursor_index;
  uint16_t top_visual_row;
  uint8_t preferred_visual_col;
  editor_overlay_t overlay;
  uint8_t command_selected_index;
  char last_search[EDITOR_LAST_SEARCH_SIZE];
  screen_id_t return_screen;  // where leaving the editor goes back to
};

struct chat_state_t {
  chat_history_t history;
  uint16_t scroll_offset;
  uint16_t total_lines;
  uint16_t newest_turn_line;  // first rendered line of the newest exchange
  bool pending;               // a question waits for its reply (or for a confirmation)
  bool follow;                // keep the reply in view while it streams
  // Agent mode: the assistant waits for the user's approval of `confirm`, no request runs.
  bool awaiting;
  char pending_prompt[CHAT_MAX_PROMPT_LENGTH + 1];
  char partial[CHAT_MAX_REPLY_LENGTH + 1];  // reply received so far
  char actions[CHAT_MAX_ACTIONS_LENGTH + 1];  // agent mode: what the assistant did so far
  chat_confirm_t confirm;
  uint16_t partial_length;
  uint16_t actions_length;
};

struct app_state_t {
  screen_id_t screen;
  screen_id_t rendered_screen;  // the screen drawn last: another one starts from a blank page
  bool full_redraw_needed;
  bool topbar_dirty;
  bool last_wifi_connected;  // false at boot: the link watch in app_loop sees the first link up
  uint8_t terminal_wake_stage;
  uint32_t terminal_wake_last_ms;
  char status_message[41];
  char shown_busy[41];  // request timer last drawn on the status line
  char last_topbar[41];
  uint32_t last_input_ms;
  uint8_t home_selected_index;
  explorer_state_t explorer;
  wifi_state_t wifi;
  settings_state_t settings_view;
  text_input_state_t text_input;
  chat_state_t chat;
  editor_state_t editor;
};

SettingsService g_settings_service;
WifiService g_wifi_service;
ClockService g_clock_service;
OtaService g_ota_service;  // network updates from a computer; server updates run on the network task
char g_chat_render_lines[CHAT_MAX_RENDER_LINES][UI_COLS + 1];
app_settings_t g_settings;
input_parser_t g_input_parser;
app_state_t g_app;

void switch_screen(screen_id_t screen);
void render_current_screen(bool until_input = false);
void start_lan_updates();

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

char invert_alpha_case_char(char ch) {
  const unsigned char value = static_cast<unsigned char>(ch);
  if (ch >= 'A' && ch <= 'Z') {
    return static_cast<char>(tolower(value));
  }
  if (ch >= 'a' && ch <= 'z') {
    return static_cast<char>(toupper(value));
  }
  return ch;
}

bool text_input_uses_inverted_alpha_case(text_input_target_t target, bool masked) {
  if (masked) {
    return false;
  }

  switch (target) {
    case TEXT_INPUT_NEW_DOCUMENT:
    case TEXT_INPUT_NEW_FOLDER:
    case TEXT_INPUT_EDIT_TAGS:
    case TEXT_INPUT_FILTER_TAG:
    case TEXT_INPUT_SEARCH_QUERY:
    case TEXT_INPUT_CHAT_PROMPT:
    case TEXT_INPUT_CALLBACK:
      return true;
    default:
      return false;
  }
}

const char* screen_label(screen_id_t screen) {
  switch (screen) {
    case SCREEN_BOOT: return "BOOT";
    case SCREEN_HOME: return "HOME";
    case SCREEN_FILES: return "FILES";
    case SCREEN_WIFI: return "WIFI";
    case SCREEN_SETTINGS: return "SETTINGS";
    case SCREEN_CHAT: return "CHAT";
    case SCREEN_EDITOR: return "EDITOR";
    case SCREEN_TEXT_INPUT: return "INPUT";
    case SCREEN_MODEL_PICKER: return "MODELE";
    case SCREEN_SYSTEM: return "SYSTEME";
    case SCREEN_PROJECTS: return "PROJETS";
    case SCREEN_PROJECT: return "PROJET";
    case SCREEN_TASKS: return "TACHES";
    case SCREEN_SEARCH: return "RECHERCHE";
    case SCREEN_LOCK: return "VERROU";
  }
  return "MINIOS";
}

const char* text_input_target_label(text_input_target_t target) {
  switch (target) {
    case TEXT_INPUT_NONE: return "NONE";
    case TEXT_INPUT_WIFI_PASSWORD: return "WIFI_PASSWORD";
    case TEXT_INPUT_API_URL: return "API_URL";
    case TEXT_INPUT_API_TOKEN: return "API_TOKEN";
    case TEXT_INPUT_AI_BASE_URL: return "AI_BASE_URL";
    case TEXT_INPUT_AI_API_KEY: return "AI_API_KEY";
    case TEXT_INPUT_TIMEZONE: return "TIMEZONE";
    case TEXT_INPUT_NEW_DOCUMENT: return "NEW_DOCUMENT";
    case TEXT_INPUT_NEW_FOLDER: return "NEW_FOLDER";
    case TEXT_INPUT_EDIT_TAGS: return "EDIT_TAGS";
    case TEXT_INPUT_FILTER_TAG: return "FILTER_TAG";
    case TEXT_INPUT_SEARCH_QUERY: return "SEARCH_QUERY";
    case TEXT_INPUT_GOTO_LINE: return "GOTO_LINE";
    case TEXT_INPUT_CHAT_PROMPT: return "CHAT_PROMPT";
    case TEXT_INPUT_CALLBACK: return "CALLBACK";
  }
  return "UNKNOWN";
}

const char* input_event_label(input_event_type_t type) {
  switch (type) {
    case INPUT_EVENT_NONE: return "NONE";
    case INPUT_EVENT_PRINTABLE: return "PRINTABLE";
    case INPUT_EVENT_ENTER: return "ENTER";
    case INPUT_EVENT_BACKSPACE: return "BACKSPACE";
    case INPUT_EVENT_ESCAPE: return "ESCAPE";
    case INPUT_EVENT_CANCEL: return "CANCEL";
    case INPUT_EVENT_SAVE: return "SAVE";
    case INPUT_EVENT_HOME: return "HOME";
    case INPUT_EVENT_END: return "END";
    case INPUT_EVENT_PAGE_UP: return "PAGE_UP";
    case INPUT_EVENT_PAGE_DOWN: return "PAGE_DOWN";
    case INPUT_EVENT_UP: return "UP";
    case INPUT_EVENT_DOWN: return "DOWN";
    case INPUT_EVENT_LEFT: return "LEFT";
    case INPUT_EVENT_RIGHT: return "RIGHT";
  }
  return "UNKNOWN";
}

const char* input_sequence_label(uint8_t state) {
  switch (state) {
    case 0: return "IDLE";
    case 1: return "SEP";
    case 2: return "SS2";
  }
  return "SEQ";
}

const char* input_byte_label(uint8_t byte) {
  switch (byte) {
    case 0x08: return "BS";
    case 0x09: return "TAB";
    case 0x0A: return "LF";
    case 0x0B: return "VT";
    case 0x0D: return "CR";
    case 0x13: return "SEP";
    case 0x19: return "SS2";
    case 0x1E: return "HOME";
  }
  return nullptr;
}

bool is_masked_input_logging() {
  return (g_app.screen == SCREEN_TEXT_INPUT && g_app.text_input.masked) || g_app.screen == SCREEN_LOCK;
}

void log_input_byte(uint8_t byte, uint8_t sequence_state) {
  if (is_masked_input_logging() && (sequence_state != 0 || (byte >= 0x20 && byte <= 0x7E))) {
    app_log_info("input-rx",
                 "screen=%s seq=%s byte=<masked>",
                 screen_label(g_app.screen),
                 input_sequence_label(sequence_state));
    return;
  }

  const char* label = input_byte_label(byte);
  if (byte == 0x00 || byte == 0xA0) {
    app_log_info("input-rx",
                 "screen=%s seq=%s byte=0x%02X space-like",
                 screen_label(g_app.screen),
                 input_sequence_label(sequence_state),
                 static_cast<unsigned>(byte));
    return;
  }

  if (byte >= 0x20 && byte <= 0x7E) {
    app_log_info("input-rx",
                 "screen=%s seq=%s byte=0x%02X char='%c'",
                 screen_label(g_app.screen),
                 input_sequence_label(sequence_state),
                 static_cast<unsigned>(byte),
                 static_cast<char>(byte));
    return;
  }

  if (label) {
    app_log_info("input-rx",
                 "screen=%s seq=%s byte=0x%02X control=%s",
                 screen_label(g_app.screen),
                 input_sequence_label(sequence_state),
                 static_cast<unsigned>(byte),
                 label);
    return;
  }

  app_log_info("input-rx",
               "screen=%s seq=%s byte=0x%02X",
               screen_label(g_app.screen),
               input_sequence_label(sequence_state),
               static_cast<unsigned>(byte));
}

void log_input_event(const input_event_t& event) {
  if (event.type == INPUT_EVENT_PRINTABLE) {
    const bool masked = is_masked_input_logging();
    if (masked) {
      app_log_info("input",
                   "screen=%s event=%s char=<masked>",
                   screen_label(g_app.screen),
                   input_event_label(event.type));
    } else {
      app_log_info("input",
                   "screen=%s event=%s char='%c' code=0x%02X",
                   screen_label(g_app.screen),
                   input_event_label(event.type),
                   event.ch,
                   static_cast<unsigned>(static_cast<uint8_t>(event.ch)));
    }
    return;
  }

  app_log_info("input", "screen=%s event=%s", screen_label(g_app.screen), input_event_label(event.type));
}

void set_status(const char* message) {
  safe_copy(g_app.status_message, sizeof(g_app.status_message), message);
  g_app.full_redraw_needed = true;
  app_log_info("status", "screen=%s message=\"%s\"", screen_label(g_app.screen), g_app.status_message);
}

void clear_status() {
  g_app.status_message[0] = '\0';
}

// Shows `message` right away, when the loop will not come back to draw it (restart).
void show_busy_status(const char* message) {
  set_status(message);
  render_current_screen();
}

// The pending request of this screen with its timer, else the last status message.
const char* status_line_text() {
  if (net::busy() && net::busy_owner() == g_app.screen) {
    net::busy_text(g_app.shown_busy, sizeof(g_app.shown_busy));
    return g_app.shown_busy;
  }
  g_app.shown_busy[0] = '\0';
  return g_app.status_message;
}

void set_status_from_reply_or(const net_reply_t& reply, const char* fallback) {
  set_status(reply.error[0] != '\0' ? reply.error : fallback);
}

bool chat_configured() {
  return url_uses_tls(g_settings.ai_base_url) && g_settings.ai_api_key[0] != '\0' && g_settings.ai_model[0] != '\0';
}

void on_store_checked(const net_request_t& request, const net_reply_t& reply) {
  if (!request.background) {
    if (!reply.ok) {
      set_status_from_reply_or(reply, TR("API indisponible", "API unavailable"));
    } else if (request.status) {
      set_status(request.status);
    }
    return;
  }
  // Silent check (boot, Wi-Fi link change): only the screens showing the server state change.
  if (!reply.ok && (g_app.screen == SCREEN_SETTINGS || g_app.screen == SCREEN_EDITOR)) {
    set_status_from_reply_or(reply, TR("API indisponible", "API unavailable"));
  } else if (g_app.screen == SCREEN_SETTINGS || g_app.screen == SCREEN_SYSTEM || g_app.screen == SCREEN_FILES) {
    g_app.full_redraw_needed = true;
  }
}

// Asks Bibliotek for /health. With a `status` (string literal) the user waits for the
// answer, which then shows; without, it is checked in the background.
void check_store(const char* status) {
  net_request_t request = net::make(NET_HEALTH, TR("Test du serveur...", "Checking server..."), on_store_checked);
  request.status = status;
  request.background = status == nullptr;
  net::submit(request);
}

void reset_terminal_wake_detector() {
  g_app.terminal_wake_stage = 0;
  g_app.terminal_wake_last_ms = 0;
}

bool is_sep_special_byte(uint8_t sequence_state, uint8_t byte, char code) {
  return sequence_state == 1 && byte == static_cast<uint8_t>(code);
}

bool is_sep_prefix_byte(uint8_t sequence_state, uint8_t byte) {
  return sequence_state == 0 && byte == 0x13;
}

bool detect_terminal_wake_sequence(uint8_t sequence_state, uint8_t byte) {
  const uint32_t now = get_millis();
  if (g_app.terminal_wake_stage != 0 &&
      now - g_app.terminal_wake_last_ms > TERMINAL_WAKE_SEQUENCE_STEP_MS) {
    reset_terminal_wake_detector();
  }

  switch (g_app.terminal_wake_stage) {
    case 0:
      if (is_sep_special_byte(sequence_state, byte, 'Y')) {
        g_app.terminal_wake_stage = 1;
        g_app.terminal_wake_last_ms = now;
      }
      return false;

    case 1:
      if (is_sep_special_byte(sequence_state, byte, 'S')) {
        g_app.terminal_wake_stage = 2;
        g_app.terminal_wake_last_ms = now;
        return false;
      }
      if (is_sep_prefix_byte(sequence_state, byte)) {
        return false;
      }
      reset_terminal_wake_detector();
      if (is_sep_special_byte(sequence_state, byte, 'Y')) {
        g_app.terminal_wake_stage = 1;
        g_app.terminal_wake_last_ms = now;
      }
      return false;

    case 2:
      if (is_sep_special_byte(sequence_state, byte, 'T')) {
        reset_terminal_wake_detector();
        return true;
      }
      if (is_sep_prefix_byte(sequence_state, byte)) {
        return false;
      }
      reset_terminal_wake_detector();
      if (is_sep_special_byte(sequence_state, byte, 'Y')) {
        g_app.terminal_wake_stage = 1;
        g_app.terminal_wake_last_ms = now;
      }
      return false;
  }

  reset_terminal_wake_detector();
  return false;
}

bool terminal_wake_detection_enabled() {
  return g_app.screen != SCREEN_EDITOR && g_app.screen != SCREEN_TEXT_INPUT;
}

void request_terminal_wake_redraw() {
  app_log_info("input", "terminal wake sequence detected -> full redraw");
  minitel_ui_force_full_refresh();
  g_app.full_redraw_needed = true;
}

void log_link_state(const char* reason) {
  const minitel_link_state_t& link = minitel_link_state();
  app_log_info("minitel",
               "%s online=%s baud=%lu routed=%s echo_filter=%s links screen=%02X socket=%02X modem=%02X",
               reason,
               link.online ? "yes" : "no",
               static_cast<unsigned long>(link.baud),
               link.routed ? "yes" : "no",
               link.echo_filter ? "on" : "off",
               link.screen_links,
               link.socket_links,
               link.modem_links);
}

// A check reads the input for its replies: the parser may be left with half a key sequence.
void handle_link_event(minitel_link_event_t event) {
  if (event == MINITEL_LINK_IDLE) {
    return;
  }
  log_link_state(event == MINITEL_LINK_RESTARTED ? "restarted" : "checked");
  input_parser_init(&g_input_parser);
  if (event == MINITEL_LINK_RESTARTED) {
    minitel_ui_force_full_refresh();
    g_app.full_redraw_needed = true;
  }
}

const char* current_screen_label() {
  return screen_label(g_app.screen);
}

void build_topbar_line(char* out, size_t out_size) {
  char wifi_label[18];
  char time_label[8];
  g_wifi_service.get_status_label(wifi_label, sizeof(wifi_label));
  g_clock_service.format_time(time_label, sizeof(time_label));
  snprintf(out, out_size, "%s | %s | %s", current_screen_label(), wifi_label, time_label);
}

void render_frame_with_current_topbar(const char* screen_name, const char* footer) {
  char wifi_label[18];
  char time_label[8];
  g_wifi_service.get_status_label(wifi_label, sizeof(wifi_label));
  g_clock_service.format_time(time_label, sizeof(time_label));
  minitel_ui_render_frame(screen_name, wifi_label, time_label, footer);
}

void request_text_input(text_input_target_t target,
                        screen_id_t return_screen,
                        const char* title,
                        const char* prompt,
                        const char* initial_value,
                        bool masked) {
  g_app.text_input.target = target;
  g_app.text_input.return_screen = return_screen;
  g_app.text_input.masked = masked;
  g_app.text_input.on_done = nullptr;
  safe_copy(g_app.text_input.title, sizeof(g_app.text_input.title), title);
  safe_copy(g_app.text_input.prompt, sizeof(g_app.text_input.prompt), prompt);
  safe_copy(g_app.text_input.buffer, sizeof(g_app.text_input.buffer), initial_value);
  g_app.text_input.length = strlen(g_app.text_input.buffer);
  g_app.text_input.cursor = g_app.text_input.length;
  g_app.screen = SCREEN_TEXT_INPUT;
  g_app.full_redraw_needed = true;
  app_log_info("nav",
               "request_input target=%s return=%s masked=%s title=\"%s\" initial_len=%u",
               text_input_target_label(target),
               screen_label(return_screen),
               masked ? "yes" : "no",
               g_app.text_input.title,
               static_cast<unsigned>(g_app.text_input.length));
}

void render_status_line() {
  minitel_ui_write_line(STATUS_ROW, status_line_text());
}

void chat_clear_session(bool show_status) {
  chat_service_utils::clear_history(&g_app.chat.history);
  g_app.chat.scroll_offset = 0;
  g_app.chat.total_lines = 0;
  g_app.chat.newest_turn_line = 0;
  g_app.chat.follow = true;
  if (show_status) {
    set_status(TR("Session chat vide", "Chat session cleared"));
  }
}

void chat_store_render_line(uint16_t* line_count, const char* text) {
  if (!line_count || *line_count >= CHAT_MAX_RENDER_LINES) {
    return;
  }

  safe_copy(g_chat_render_lines[*line_count], sizeof(g_chat_render_lines[0]), text ? text : "");
  ++(*line_count);
}

void chat_store_render_line_sink(const char* line, void* context) {
  chat_store_render_line(static_cast<uint16_t*>(context), line);
}

void chat_append_wrapped_message(const char* prefix,
                                 const char* continuation_prefix,
                                 const char* text,
                                 uint16_t* line_count) {
  text_wrap::wrap(text, prefix, continuation_prefix, UI_COLS, chat_store_render_line_sink, line_count);
}

// Agent mode: one line per action the assistant ran. Counts only without `line_count`.
uint16_t chat_append_actions(const char* actions, uint16_t* line_count) {
  return text_wrap::wrap_list(actions,
                              CHAT_ACTION_PREFIX,
                              CHAT_CONTINUATION_PREFIX,
                              UI_COLS,
                              line_count ? chat_store_render_line_sink : nullptr,
                              line_count);
}

uint16_t chat_turn_line_count(const chat_turn_t& turn) {
  uint16_t lines = text_wrap::wrap(turn.user, "Q> ", "   ", UI_COLS, nullptr, nullptr);
  lines = static_cast<uint16_t>(lines + chat_append_actions(turn.actions, nullptr));
  lines = static_cast<uint16_t>(lines + text_wrap::wrap(turn.assistant, "R> ", "   ", UI_COLS, nullptr, nullptr));
  return turn.assistant_truncated ? static_cast<uint16_t>(lines + 1) : lines;
}

// What arrived of the pending reply, "..." while nothing did; none while the assistant
// waits for the user's approval without having written anything.
const char* chat_pending_reply() {
  if (g_app.chat.partial[0] != '\0') {
    return g_app.chat.partial;
  }
  return g_app.chat.awaiting ? nullptr : "...";
}

const char* chat_confirm_text() {
  return g_app.chat.confirm.text[0] != '\0' ? g_app.chat.confirm.text : TR("Action a confirmer", "Action to confirm");
}

// The question waiting for its reply: the actions so far, what arrived of the reply, and the
// action the assistant asks about.
uint16_t chat_pending_line_count() {
  uint16_t lines = text_wrap::wrap(g_app.chat.pending_prompt, "Q> ", "   ", UI_COLS, nullptr, nullptr);
  lines = static_cast<uint16_t>(lines + chat_append_actions(g_app.chat.actions, nullptr));
  const char* reply = chat_pending_reply();
  if (reply) {
    lines = static_cast<uint16_t>(lines + text_wrap::wrap(reply, "R> ", "   ", UI_COLS, nullptr, nullptr));
  }
  if (g_app.chat.awaiting) {
    lines = static_cast<uint16_t>(
        lines + text_wrap::wrap(chat_confirm_text(), CHAT_CONFIRM_PREFIX, CHAT_CONTINUATION_PREFIX, UI_COLS, nullptr, nullptr));
  }
  return lines;
}

// Oldest turn to render so that the newest exchanges always fit in the render buffer.
uint8_t chat_first_rendered_turn() {
  uint16_t needed = g_app.chat.pending ? static_cast<uint16_t>(chat_pending_line_count() + 1) : 0;
  uint8_t first = g_app.chat.history.count;
  while (first > 0) {
    // +1 for the blank separator, +1 for the "hidden turns" marker if older turns are dropped.
    const uint16_t turn_lines =
        static_cast<uint16_t>(chat_turn_line_count(g_app.chat.history.turns[first - 1]) + 2);
    if (first < g_app.chat.history.count && needed + turn_lines > CHAT_MAX_RENDER_LINES) {
      break;
    }
    needed = static_cast<uint16_t>(needed + turn_lines);
    --first;
  }
  return first;
}

void chat_rebuild_render_lines() {
  memset(g_chat_render_lines, 0, sizeof(g_chat_render_lines));

  uint16_t line_count = 0;
  if (g_app.chat.history.count == 0 && !g_app.chat.pending) {
    chat_append_wrapped_message(
        "", "", TR("Session vide. ENVOI pour poser une question.", "Empty session. ENVOI to ask a question."),
        &line_count);
  }

  const uint8_t first_turn = chat_first_rendered_turn();
  if (first_turn > 0) {
    chat_store_render_line(&line_count, TR("[echanges precedents masques]", "[earlier exchanges hidden]"));
  }

  g_app.chat.newest_turn_line = line_count;
  for (uint8_t i = first_turn; i < g_app.chat.history.count && line_count < CHAT_MAX_RENDER_LINES; ++i) {
    const chat_turn_t& turn = g_app.chat.history.turns[i];
    g_app.chat.newest_turn_line = line_count;
    chat_append_wrapped_message("Q> ", "   ", turn.user, &line_count);
    chat_append_actions(turn.actions, &line_count);
    chat_append_wrapped_message("R> ", "   ", turn.assistant, &line_count);
    if (turn.assistant_truncated) {
      chat_store_render_line(&line_count, TR("   [reponse tronquee]", "   [reply truncated]"));
    }
    if ((i + 1 < g_app.chat.history.count || g_app.chat.pending) &&
        line_count < CHAT_MAX_RENDER_LINES) {
      chat_store_render_line(&line_count, "");
    }
  }

  if (g_app.chat.pending && line_count < CHAT_MAX_RENDER_LINES) {
    g_app.chat.newest_turn_line = line_count;
    chat_append_wrapped_message("Q> ", "   ", g_app.chat.pending_prompt, &line_count);
    chat_append_actions(g_app.chat.actions, &line_count);
    const char* reply = chat_pending_reply();
    if (reply) {
      chat_append_wrapped_message("R> ", "   ", reply, &line_count);
    }
    if (g_app.chat.awaiting) {
      chat_append_wrapped_message(CHAT_CONFIRM_PREFIX, CHAT_CONTINUATION_PREFIX, chat_confirm_text(), &line_count);
    }
  }

  g_app.chat.total_lines = line_count;
}

uint16_t chat_bottom_offset() {
  return g_app.chat.total_lines > CHAT_VISIBLE_ROWS
             ? static_cast<uint16_t>(g_app.chat.total_lines - CHAT_VISIBLE_ROWS)
             : 0;
}

// The last line may sit anywhere on the screen (see chat_follow_reply), not below it.
void chat_ensure_scroll_in_bounds() {
  uint16_t max_offset = g_app.chat.total_lines > 0 ? static_cast<uint16_t>(g_app.chat.total_lines - 1) : 0;
  if (max_offset < chat_bottom_offset()) {
    max_offset = chat_bottom_offset();
  }
  if (g_app.chat.scroll_offset > max_offset) {
    g_app.chat.scroll_offset = max_offset;
  }
}

void chat_scroll_to_bottom() {
  chat_rebuild_render_lines();
  g_app.chat.scroll_offset = chat_bottom_offset();
}

// While a reply streams, keeps its last line in view a page at a time: scrolling line by
// line would resend the whole transcript to the Minitel for each new line.
void chat_follow_reply() {
  if (!g_app.chat.pending || !g_app.chat.follow || g_app.chat.total_lines == 0) {
    return;
  }
  const uint16_t last_line = static_cast<uint16_t>(g_app.chat.total_lines - 1);
  if (last_line >= g_app.chat.scroll_offset && last_line < g_app.chat.scroll_offset + CHAT_VISIBLE_ROWS) {
    return;
  }
  uint16_t top = last_line > CHAT_PAGE_CONTEXT_LINES ? static_cast<uint16_t>(last_line - CHAT_PAGE_CONTEXT_LINES) : 0;
  if (g_app.chat.newest_turn_line < top && last_line - g_app.chat.newest_turn_line < CHAT_VISIBLE_ROWS) {
    top = g_app.chat.newest_turn_line;  // the whole exchange fits: start the page with the question
  }
  g_app.chat.scroll_offset = top;
}

// The assistant with tools runs on Bibliotek: used when it is configured and not turned off.
bool chat_agent_mode() {
  return g_settings.ai_tools && g_settings.api_url[0] != '\0' && g_settings.api_token[0] != '\0';
}

void chat_request_prompt_input() {
  request_text_input(TEXT_INPUT_CHAT_PROMPT,
                     SCREEN_CHAT,
                     TR("Chat IA", "AI chat"),
                     chat_agent_mode() ? TR("Question (l'IA voit tes notes)", "Question (the AI sees your notes)")
                                       : TR("Question pour Gradient", "Question for Gradient"),
                     "",
                     false);
}

// Network task input, copied when the request starts: the remembered exchanges for a
// question; for a confirmation, the reply and the actions so far, which it goes on after.
void prepare_chat(net_request_t& request, net_reply_t& reply) {
  auto& chat = reply.data.chat;
  memset(&chat.confirm, 0, sizeof(chat.confirm));
  if (request.kind == NET_CHAT_CONFIRM) {
    safe_copy(chat.text, sizeof(chat.text), g_app.chat.partial);
    safe_copy(chat.actions, sizeof(chat.actions), g_app.chat.actions);
    return;
  }
  chat.history = g_app.chat.history;
  chat.text[0] = '\0';
  chat.actions[0] = '\0';
}

// Ends the pending exchange with `reply` and the actions so far. Nothing is kept when the
// assistant neither answered nor acted. The view stays where it is, even when the oldest
// exchange leaves the bounded history.
void chat_finish_exchange(const char* reply, bool truncated) {
  const int32_t from_newest =
      static_cast<int32_t>(g_app.chat.scroll_offset) - static_cast<int32_t>(g_app.chat.newest_turn_line);
  const bool answered = reply && reply[0] != '\0';
  const char* text = answered ? reply : (g_app.chat.actions[0] != '\0' ? CHAT_NO_REPLY : nullptr);
  const bool kept = text && chat_service_utils::append_turn(&g_app.chat.history,
                                                            g_app.chat.pending_prompt,
                                                            text,
                                                            g_app.chat.actions,
                                                            truncated && answered);
  g_app.chat.pending = false;
  g_app.chat.awaiting = false;
  g_app.chat.partial[0] = '\0';
  g_app.chat.partial_length = 0;
  g_app.chat.actions[0] = '\0';
  g_app.chat.actions_length = 0;
  memset(&g_app.chat.confirm, 0, sizeof(g_app.chat.confirm));
  chat_rebuild_render_lines();
  if (kept) {
    const int32_t offset = static_cast<int32_t>(g_app.chat.newest_turn_line) + from_newest;
    g_app.chat.scroll_offset = offset > 0 ? static_cast<uint16_t>(offset) : 0;
  }
  chat_ensure_scroll_in_bounds();
  g_app.full_redraw_needed = true;
}

void on_chat_reply(const net_request_t& request, const net_reply_t& reply) {
  (void)request;
  if (!g_app.chat.pending) {
    return;
  }
  const auto& chat = reply.data.chat;
  safe_copy(g_app.chat.actions, sizeof(g_app.chat.actions), chat.actions);
  if (reply.ok && chat.confirm.id[0] != '\0') {
    // The assistant waits for the user: the exchange goes on once they answered.
    safe_copy(g_app.chat.partial, sizeof(g_app.chat.partial), chat.text);
    g_app.chat.partial_length = static_cast<uint16_t>(strlen(g_app.chat.partial));
    g_app.chat.actions_length = static_cast<uint16_t>(strlen(g_app.chat.actions));
    g_app.chat.confirm = chat.confirm;
    g_app.chat.awaiting = true;
    g_app.chat.follow = true;
    set_status(TR("L'IA attend ton accord", "The AI awaits your OK"));
    return;
  }
  if (reply.ok) {
    chat_finish_exchange(chat.text, reply.truncated);
    set_status(reply.truncated ? TR("Reponse recue (tronquee)", "Reply received (truncated)")
                               : TR("Reponse recue", "Reply received"));
    return;
  }
  chat_finish_exchange(chat.text, true);  // cut midway: keep what arrived
  set_status_from_reply_or(reply, TR("Requete IA KO", "AI request failed"));
}

// What arrived stays: chat_poll_reply() copied it on this loop iteration.
void on_chat_cancelled(const net_request_t& request) {
  (void)request;
  if (!g_app.chat.pending) {
    return;
  }
  const bool kept = g_app.chat.partial[0] != '\0' || g_app.chat.actions[0] != '\0';
  chat_finish_exchange(g_app.chat.partial, true);
  set_status(kept ? TR("Reponse interrompue", "Reply interrupted")
                  : TR("Requete IA annulee", "AI request cancelled"));
}

void format_local_now(char* out, size_t out_size) {
  struct tm local;
  if (!out || out_size == 0) {
    return;
  }
  out[0] = '\0';
  if (g_clock_service.local_time(&local)) {
    strftime(out, out_size, "%Y-%m-%d %H:%M", &local);
  }
}

void chat_submit_prompt(const char* prompt) {
  switch_screen(SCREEN_CHAT);
  char normalized[CHAT_MAX_PROMPT_LENGTH + 1];
  bool truncated = false;
  if (!chat_service_utils::normalize_text(prompt, normalized, sizeof(normalized), &truncated)) {
    set_status(TR("Prompt vide", "Empty prompt"));
    return;
  }

  net_request_t request = net::make(NET_CHAT, TR("Requete IA...", "AI request..."), on_chat_reply);
  request.on_cancelled = on_chat_cancelled;
  request.prepare = prepare_chat;
  safe_copy(request.params.chat.prompt, sizeof(request.params.chat.prompt), normalized);
  request.params.chat.agent = chat_agent_mode();
  if (request.params.chat.agent) {
    format_local_now(request.params.chat.now, sizeof(request.params.chat.now));
  }
  if (!net::submit(request)) {
    return;
  }
  safe_copy(g_app.chat.pending_prompt, sizeof(g_app.chat.pending_prompt), normalized);
  g_app.chat.partial[0] = '\0';
  g_app.chat.partial_length = 0;
  g_app.chat.actions[0] = '\0';
  g_app.chat.actions_length = 0;
  memset(&g_app.chat.confirm, 0, sizeof(g_app.chat.confirm));
  g_app.chat.awaiting = false;
  g_app.chat.pending = true;
  g_app.chat.follow = true;
  chat_rebuild_render_lines();
  chat_follow_reply();
  g_app.full_redraw_needed = true;
}

// ENVOI (accept) or CORRECTION (refuse) while the assistant waits: it goes on with the
// action, or hears the refusal.
void chat_answer_confirmation(bool accept) {
  net_request_t request =
      net::make(NET_CHAT_CONFIRM,
                accept ? TR("Action acceptee...", "Action accepted...") : TR("Action refusee...", "Action refused..."),
                on_chat_reply);
  request.on_cancelled = on_chat_cancelled;
  request.prepare = prepare_chat;
  safe_copy(request.params.confirm.id, sizeof(request.params.confirm.id), g_app.chat.confirm.id);
  request.params.confirm.accept = accept;
  if (!net::submit(request)) {
    return;
  }
  g_app.chat.awaiting = false;
  g_app.chat.follow = true;
  clear_status();
  chat_rebuild_render_lines();
  chat_follow_reply();
  g_app.full_redraw_needed = true;
}

// ANNULATION while the assistant waits: the exchange ends there, without the action.
void chat_abandon_confirmation() {
  const bool wrote = g_app.chat.partial[0] != '\0';
  chat_finish_exchange(wrote ? g_app.chat.partial : TR("(action abandonnee)", "(action dropped)"), wrote);
  set_status(TR("Action abandonnee", "Action dropped"));
}

// The reply or the actions grew: redraw the chat with them.
void chat_poll_reply() {
  if (!g_app.chat.pending || g_app.chat.awaiting) {
    return;
  }
  if (!net::chat_progress(g_app.chat.partial, sizeof(g_app.chat.partial), g_app.chat.actions,
                          sizeof(g_app.chat.actions))) {
    return;  // queued, or over: keep what arrived
  }
  const uint16_t partial_length = static_cast<uint16_t>(strlen(g_app.chat.partial));
  const uint16_t actions_length = static_cast<uint16_t>(strlen(g_app.chat.actions));
  if (partial_length != g_app.chat.partial_length || actions_length != g_app.chat.actions_length) {
    g_app.chat.partial_length = partial_length;
    g_app.chat.actions_length = actions_length;
    if (g_app.screen == SCREEN_CHAT) {
      g_app.full_redraw_needed = true;
    }
  }
}

void ensure_explorer_selection_visible() {
  if (g_app.explorer.selected_index < g_app.explorer.scroll_offset) {
    g_app.explorer.scroll_offset = g_app.explorer.selected_index;
  } else if (g_app.explorer.selected_index >= g_app.explorer.scroll_offset + FILES_VISIBLE_ROWS) {
    g_app.explorer.scroll_offset = g_app.explorer.selected_index - FILES_VISIBLE_ROWS + 1;
  }
}

void explorer_select_folder(const char* path) {
  for (uint8_t i = 0; i < g_app.explorer.item_count; ++i) {
    if (g_app.explorer.items[i].kind == EXPLORER_ITEM_FOLDER &&
        strcmp(g_app.explorer.items[i].path, path) == 0) {
      g_app.explorer.selected_index = i;
      ensure_explorer_selection_visible();
      return;
    }
  }
}

// Shows the listing of `request`'s folder: the explorer only moves once it arrived.
void on_explorer_listed(const net_request_t& request, const net_reply_t& reply) {
  if (!reply.ok) {
    set_status_from_reply_or(reply, TR("API indisponible", "API unavailable"));
    return;
  }

  const bool same_folder = strcmp(g_app.explorer.current_folder, request.params.listing.folder) == 0;
  safe_copy(g_app.explorer.current_folder, sizeof(g_app.explorer.current_folder), request.params.listing.folder);
  safe_copy(g_app.explorer.tag_filter, sizeof(g_app.explorer.tag_filter), request.params.listing.tag);
  if (!same_folder) {
    g_app.explorer.selected_index = 0;
    g_app.explorer.scroll_offset = 0;
  }

  const int folder_count = reply.data.listing.folder_count;
  const int doc_count = reply.data.listing.document_count;
  g_app.explorer.item_count = 0;
  for (int i = 0; i < folder_count && g_app.explorer.item_count < MAX_EXPLORER_ITEMS; ++i) {
    const folder_info_t& folder = reply.data.listing.folders[i];
    explorer_item_t& item = g_app.explorer.items[g_app.explorer.item_count++];
    item.kind = EXPLORER_ITEM_FOLDER;
    snprintf(item.label, sizeof(item.label), "[%s]", folder.name);
    safe_copy(item.path, sizeof(item.path), folder.path);
    item.document_id[0] = '\0';
    safe_copy(item.meta, sizeof(item.meta), "folder");
  }

  for (int i = 0; i < doc_count && g_app.explorer.item_count < MAX_EXPLORER_ITEMS; ++i) {
    const document_summary_t& document = reply.data.listing.documents[i];
    explorer_item_t& item = g_app.explorer.items[g_app.explorer.item_count++];
    item.kind = EXPLORER_ITEM_DOCUMENT;
    snprintf(item.label, sizeof(item.label), "%s.%s", document.title, document.extension);
    safe_copy(item.path, sizeof(item.path), document.folder_path);
    safe_copy(item.document_id, sizeof(item.document_id), document.id);
    safe_copy(item.meta, sizeof(item.meta), document.tags_summary);
  }

  if (g_app.explorer.selected_index >= g_app.explorer.item_count && g_app.explorer.item_count > 0) {
    g_app.explorer.selected_index = g_app.explorer.item_count - 1;
  }
  if (g_app.explorer.item_count == 0) {
    g_app.explorer.selected_index = 0;
    g_app.explorer.scroll_offset = 0;
  }
  // The list may have shrunk: keep the selection inside the visible window.
  ensure_explorer_selection_visible();
  if (request.params.listing.select[0] != '\0') {
    explorer_select_folder(request.params.listing.select);
  }
  g_app.explorer.stale = false;
  if (request.status) {
    set_status(request.status);
  }
  g_app.full_redraw_needed = true;
  app_log_info("files",
               "listed folder=\"%s\" filter=\"%s\" folders=%d docs=%d items=%u",
               g_app.explorer.current_folder,
               g_app.explorer.tag_filter[0] ? g_app.explorer.tag_filter : "<none>",
               folder_count,
               doc_count,
               static_cast<unsigned>(g_app.explorer.item_count));
}

// Lists `folder` with `tag` (folders, then the documents carrying the tag). `select` names a
// folder to select once listed; `status` (string literal) shows when it arrived.
void request_explorer_listing(const char* folder, const char* tag, const char* select, const char* status) {
  net_request_t request =
      net::make(NET_LIST_FOLDER, TR("Chargement du dossier...", "Loading folder..."), on_explorer_listed);
  safe_copy(request.params.listing.folder, sizeof(request.params.listing.folder), folder);
  safe_copy(request.params.listing.tag, sizeof(request.params.listing.tag), tag);
  safe_copy(request.params.listing.select, sizeof(request.params.listing.select), select);
  request.status = status;
  net::submit(request);
}

void refresh_explorer(const char* status) {
  request_explorer_listing(g_app.explorer.current_folder, g_app.explorer.tag_filter, nullptr, status);
}

// The document arrived: opens it in the editor.
void show_document(const document_record_t& record, uint16_t line, screen_id_t return_screen) {
  g_app.editor.active = true;
  g_app.editor.dirty = false;
  g_app.editor.document = record;
  g_app.editor.cursor_index = 0;
  g_app.editor.top_visual_row = 0;
  g_app.editor.preferred_visual_col = 0;
  g_app.editor.overlay = EDITOR_OVERLAY_NONE;
  g_app.editor.command_selected_index = 0;
  g_app.editor.last_search[0] = '\0';
  g_app.editor.return_screen = return_screen;
  if (line > 0) {
    g_app.editor.cursor_index = editor_layout::index_from_logical_line(record.body, static_cast<uint16_t>(line - 1));
  }
  safe_copy(g_settings.last_document_id, sizeof(g_settings.last_document_id), record.id);
  g_settings_service.save(g_settings);
  g_app.screen = SCREEN_EDITOR;
  g_app.full_redraw_needed = true;
  clear_status();
  if (record.truncated) {
    set_status(TR("Lecture seule: doc trop long", "Read-only: document too long"));
  }
  char recent_title[sizeof(workspace::recent_document_t::title)];
  snprintf(recent_title, sizeof(recent_title), "%s.%s", record.title, record.extension);
  workspace::recent_add(record.id, recent_title);
  app_log_info("editor",
               "open_document id=\"%s\" title=\"%s.%s\" body_len=%u",
               record.id,
               record.title,
               record.extension,
               static_cast<unsigned>(strlen(record.body)));
}

void on_document_loaded(const net_request_t& request, const net_reply_t& reply) {
  if (!reply.ok) {
    set_status_from_reply_or(reply, TR("Document introuvable", "Document not found"));
    return;
  }
  show_document(reply.data.document, request.params.open.line, static_cast<screen_id_t>(request.params.open.return_screen));
}

// Loads `id` and opens it in the editor (see shell::open_document_from).
void request_document(const char* id, uint16_t line, screen_id_t return_screen) {
  net_request_t request = net::make(NET_GET_DOCUMENT, TR("Ouverture...", "Opening..."), on_document_loaded);
  safe_copy(request.params.open.id, sizeof(request.params.open.id), id);
  request.params.open.line = line;
  request.params.open.return_screen = static_cast<uint8_t>(return_screen);
  net::submit(request);
}

void render_boot_screen(const char* line1, const char* line2) {
  static bool boot_screen_initialized = false;
  app_log_info("boot", "screen line1=\"%s\" line2=\"%s\"", line1 ? line1 : "", line2 ? line2 : "");

  if (!boot_screen_initialized) {
    minitel_ui_render_frame("BOOT", "wifi...", "--:--", "");
    minitel_ui_write_line(BOOT_STATUS_LINE1_ROW, "");
    minitel_ui_write_line(BOOT_STATUS_LINE2_ROW, "");
    minitel_ui_flush();
    minitel_ui_draw_mosaic_art(BOOT_LOGO_ROW,
                               BOOT_LOGO_COL,
                               &BOOT_LOGO_MASKS[0][0],
                               BOOT_LOGO_WIDTH,
                               BOOT_LOGO_HEIGHT);
    boot_screen_initialized = true;
  }

  minitel_ui_write_line(BOOT_STATUS_LINE1_ROW, line1);
  minitel_ui_write_line(BOOT_STATUS_LINE2_ROW, line2);
  minitel_ui_flush_rows(BOOT_STATUS_LINE1_ROW, BOOT_STATUS_LINE2_ROW);
}

void render_home_screen() {
  render_frame_with_current_topbar(TR("ACCUEIL", "HOME"),
                                   TR("ENVOI ouvrir  1-9 acces direct", "ENVOI open  1-9 direct access"));
  char line[41];

  struct tm now;
  if (g_clock_service.local_time(&now)) {
    snprintf(line, sizeof(line), "%s %d %s %d", DAY_NAMES[now.tm_wday], now.tm_mday, MONTH_NAMES[now.tm_mon],
             now.tm_year + 1900);
  } else {
    snprintf(line, sizeof(line), TR("Heure pas encore synchronisee", "Clock not synced yet"));
  }
  minitel_ui_write_line(3, line);

  if (workspace::today_known()) {
    const uint16_t open = workspace::today_open_count();
    const bool plural = TR(open > 1, open != 1);  // French counts 0 as singular, English as plural
    snprintf(line, sizeof(line), TR("Aujourd'hui: %u tache%s, %u en retard", "Today: %u task%s, %u late"), open,
             plural ? "s" : "", workspace::today_overdue_count());
  } else {
    snprintf(line, sizeof(line), TR("Aujourd'hui: taches pas encore lues", "Today: tasks not loaded yet"));
  }
  minitel_ui_write_line(4, line);
  if (workspace::timer_running()) {
    const uint32_t minutes = workspace::timer_elapsed_minutes();
    snprintf(line, sizeof(line), TR("Chrono: %.24s %luh%02lu", "Timer: %.24s %luh%02lu"), workspace::timer_project(),
             static_cast<unsigned long>(minutes / 60), static_cast<unsigned long>(minutes % 60));
    minitel_ui_write_line(5, line);
  }

  for (uint8_t i = 0; i < HOME_MENU_COUNT; ++i) {
    snprintf(line, sizeof(line), "%c %u. %s", i == g_app.home_selected_index ? '>' : ' ', i + 1,
             HOME_MENU_LABELS[i]);
    minitel_ui_write_line(static_cast<uint8_t>(HOME_MENU_START_ROW + i), line);
  }
  const uint8_t recents = workspace::recent_count();
  if (recents > 0) {
    minitel_ui_write_line(HOME_RECENTS_ROW, TR("Recents:", "Recent:"));
    for (uint8_t i = 0; i < recents; ++i) {
      snprintf(line, sizeof(line), "%c    %.34s", HOME_MENU_COUNT + i == g_app.home_selected_index ? '>' : ' ',
               workspace::recent_at(i).title);
      minitel_ui_write_line(static_cast<uint8_t>(HOME_RECENTS_ROW + 1 + i), line);
    }
  }
  render_status_line();
}

void render_files_screen() {
  const bool at_root = strcmp(g_app.explorer.current_folder, "/") == 0;
  const char* footer = at_root ? "ENVOI open REP/SUITE nav D/F" : "ENVOI open REP/SUITE nav N/D/T/F";
  render_frame_with_current_topbar("FILES", footer);

  char folder_line[41];
  snprintf(folder_line, sizeof(folder_line), TR("Dossier: %s", "Folder: %s"), g_app.explorer.current_folder);
  minitel_ui_write_line(3, folder_line);

  char filter_line[41];
  snprintf(filter_line,
           sizeof(filter_line),
           TR("Filtre tag: %s", "Tag filter: %s"),
           g_app.explorer.tag_filter[0] ? g_app.explorer.tag_filter : TR("aucun", "none"));
  minitel_ui_write_line(4, filter_line);

  for (uint8_t row = 0; row < FILES_VISIBLE_ROWS; ++row) {
    uint8_t item_index = g_app.explorer.scroll_offset + row;
    char line[41];
    if (item_index < g_app.explorer.item_count) {
      const explorer_item_t& item = g_app.explorer.items[item_index];
      snprintf(line,
               sizeof(line),
               "%c %s",
               item_index == g_app.explorer.selected_index ? '>' : ' ',
               item.label);
      minitel_ui_write_line(FILES_LIST_START_ROW + row, line);
    } else {
      minitel_ui_write_line(FILES_LIST_START_ROW + row, "");
    }
  }

  if (g_app.explorer.item_count == 0) {
    if (net::store_available()) {
      minitel_ui_write_line(8, at_root ? TR("Aucun dossier. D pour creer.", "No folder. D to create one.")
                                       : TR("Aucun element. N ou D.", "Nothing here. N or D."));
    } else {
      minitel_ui_write_line(8, TR("API indisponible.", "API unavailable."));
    }
    minitel_ui_write_line(FILES_META_ROW, net::store_available() ? "" : net::store_error());
  } else {
    minitel_ui_write_line(FILES_META_ROW, g_app.explorer.items[g_app.explorer.selected_index].meta);
  }

  minitel_ui_write_line(FILES_HELP_ROW, "SOM=left GUI=right RETOUR parent");
  render_status_line();
}

void render_wifi_screen() {
  const char* footer = "ENVOI connect REP/SUITE nav R scan";
  render_frame_with_current_topbar("WIFI", footer);

  char ssid[SETTINGS_MAX_WIFI_SSID + 1];
  char ip[20];
  g_wifi_service.get_ssid(ssid, sizeof(ssid));
  g_wifi_service.get_ip(ip, sizeof(ip));

  char line1[41];
  snprintf(line1, sizeof(line1), TR("Actuel: %s", "Current: %s"), ssid);
  minitel_ui_write_line(3, line1);
  char line2[41];
  snprintf(line2, sizeof(line2), "IP: %s", ip);
  minitel_ui_write_line(4, line2);

  char line3[41];
  snprintf(line3, sizeof(line3), TR("Defaut: %s", "Saved: %s"),
           g_settings.wifi_ssid[0] ? g_settings.wifi_ssid : TR("aucun", "none"));
  minitel_ui_write_line(5, line3);

  for (uint8_t row = 0; row < WIFI_VISIBLE_ROWS; ++row) {
    uint8_t network_index = g_app.wifi.scroll_offset + row;
    char line[41];
    if (network_index < g_app.wifi.network_count) {
      const lm_wifi_network_t& network = g_app.wifi.networks[network_index];
      snprintf(line,
               sizeof(line),
               "%c %s %s",
               network_index == g_app.wifi.selected_index ? '>' : ' ',
               network.ssid,
               network.encrypted ? "*" : " ");
      minitel_ui_write_line(WIFI_LIST_START_ROW + row, line);
    } else {
      minitel_ui_write_line(WIFI_LIST_START_ROW + row, "");
    }
  }

  minitel_ui_write_line(WIFI_INFO_ROW, TR("* = mot de passe requis", "* = password required"));
  render_status_line();
}

void render_settings_screen() {
  const char* footer = TR("ENVOI ouvrir/edit REP/SUITE nav RETOUR", "ENVOI open/edit REP/SUITE nav RETOUR");
  render_frame_with_current_topbar("SETTINGS", footer);

  char line[41];
  minitel_ui_write_line(3, g_app.settings_view.selected_index == 0 ? "> WiFi" : "  WiFi");

  snprintf(line,
           sizeof(line),
           "%c Docs URL: %.28s",
           g_app.settings_view.selected_index == 1 ? '>' : ' ',
           g_settings.api_url);
  minitel_ui_write_line(4, line);

  snprintf(line,
           sizeof(line),
           "%c Docs token: %s",
           g_app.settings_view.selected_index == 2 ? '>' : ' ',
           g_settings.api_token[0] ? TR("configure", "set") : TR("vide", "empty"));
  minitel_ui_write_line(5, line);

  snprintf(line,
           sizeof(line),
           TR("%c IA URL: %.30s", "%c AI URL: %.30s"),
           g_app.settings_view.selected_index == 3 ? '>' : ' ',
           g_settings.ai_base_url);
  minitel_ui_write_line(6, line);

  snprintf(line,
           sizeof(line),
           TR("%c IA key: %s", "%c AI key: %s"),
           g_app.settings_view.selected_index == 4 ? '>' : ' ',
           g_settings.ai_api_key[0] ? TR("configuree", "set") : TR("vide", "empty"));
  minitel_ui_write_line(7, line);

  snprintf(line,
           sizeof(line),
           TR("%c IA modele: %.26s", "%c AI model: %.26s"),
           g_app.settings_view.selected_index == 5 ? '>' : ' ',
           g_settings.ai_model);
  minitel_ui_write_line(8, line);

  // The assistant's tools run on Bibliotek: without its URL and token, the chat stays direct.
  const char* tools_state = TR("coupes", "off");
  if (g_settings.ai_tools) {
    tools_state = g_settings.api_url[0] && g_settings.api_token[0] ? TR("actifs (notes)", "on (notes)")
                                                                   : TR("actifs, sans Docs", "on, no Docs");
  }
  snprintf(line,
           sizeof(line),
           TR("%c Outils IA: %s", "%c AI tools: %s"),
           g_app.settings_view.selected_index == SETTINGS_AI_TOOLS_INDEX ? '>' : ' ',
           tools_state);
  minitel_ui_write_line(9, line);

  snprintf(line,
           sizeof(line),
           "%c Timezone: %.22s",
           g_app.settings_view.selected_index == SETTINGS_TIMEZONE_INDEX ? '>' : ' ',
           g_settings.timezone);
  minitel_ui_write_line(10, line);

  const char* reset_line = "  Reset settings";
  if (g_app.settings_view.selected_index == SETTINGS_RESET_INDEX) {
    reset_line = g_app.settings_view.reset_armed
                     ? TR("> Reset settings ? ENVOI confirme", "> Reset settings ? ENVOI confirms")
                     : "> Reset settings";
  }
  snprintf(line,
           sizeof(line),
           TR("%c Code PIN: %s", "%c PIN code: %s"),
           g_app.settings_view.selected_index == SETTINGS_PIN_INDEX ? '>' : ' ',
           lock_screen_required() ? TR("actif", "on") : TR("aucun", "none"));
  minitel_ui_write_line(11, line);
  minitel_ui_write_line(12, reset_line);
  minitel_ui_write_line(14, g_clock_service.is_synced() ? TR("Heure NTP synchronisee", "NTP time synced")
                                                        : TR("Heure NTP en attente", "NTP time pending"));

  snprintf(line,
           sizeof(line),
           "Docs: %.34s",
           net::store_available() ? TR("Bibliotek connectee", "Bibliotek connected") : net::store_error());
  minitel_ui_write_line(15, line);

  const char* chat_status = TR("config incomplete", "setup incomplete");
  if (net::chat_error()[0] != '\0') {
    chat_status = net::chat_error();
  } else if (chat_configured()) {
    chat_status = TR("prete", "ready");
  }
  snprintf(line, sizeof(line), TR("IA: %.36s", "AI: %.36s"), chat_status);
  minitel_ui_write_line(16, line);
  render_status_line();
}

void render_chat_screen() {
  const char* footer = g_app.chat.awaiting
                           ? TR("ENVOI oui  CORRECTION non  ANN abandon", "ENVOI yes  CORRECTION no  ANN drop")
                           : TR("ENVOI prompt REP/SUITE RET home ANN raz", "ENVOI prompt REP/SUITE RET home ANN clear");
  render_frame_with_current_topbar("CHAT", footer);

  char model_line[41];
  const char* model = g_settings.ai_model[0] ? g_settings.ai_model : TR("non configure", "not set");
  if (chat_agent_mode()) {
    snprintf(model_line, sizeof(model_line), TR("Modele: %.24s +outils", "Model: %.24s +tools"), model);
  } else {
    snprintf(model_line, sizeof(model_line), TR("Modele: %.31s", "Model: %.31s"), model);
  }
  minitel_ui_write_line(CHAT_MODEL_ROW, model_line);

  chat_rebuild_render_lines();
  chat_follow_reply();
  chat_ensure_scroll_in_bounds();
  for (uint8_t row = 0; row < CHAT_VISIBLE_ROWS; ++row) {
    const uint16_t line_index = static_cast<uint16_t>(g_app.chat.scroll_offset + row);
    minitel_ui_write_line(
        static_cast<uint8_t>(CHAT_TRANSCRIPT_START_ROW + row),
        line_index < g_app.chat.total_lines ? g_chat_render_lines[line_index] : "");
  }

  render_status_line();
}

void render_text_input_screen() {
  const char* footer = TR("ENVOI valider SOM/GUI curseur RETOUR", "ENVOI confirm SOM/GUI cursor RETOUR");
  render_frame_with_current_topbar("INPUT", footer);
  minitel_ui_write_line(3, g_app.text_input.title);
  minitel_ui_write_line(4, g_app.text_input.prompt);

  char visible[TEXT_INPUT_VISIBLE_COLUMNS + 1];
  memset(visible, 0, sizeof(visible));
  uint16_t start = 0;
  if (g_app.text_input.cursor > TEXT_INPUT_VISIBLE_COLUMNS - 2) {
    start = static_cast<uint16_t>(g_app.text_input.cursor - (TEXT_INPUT_VISIBLE_COLUMNS - 2));
  }

  uint8_t out_index = 0;
  for (uint16_t i = start; i < g_app.text_input.length && out_index < TEXT_INPUT_VISIBLE_COLUMNS; ++i) {
    visible[out_index++] = g_app.text_input.masked ? '*' : g_app.text_input.buffer[i];
  }
  visible[out_index] = '\0';
  minitel_ui_write_line(TEXT_INPUT_BUFFER_ROW, visible);
  render_status_line();
}

int editor_body_length() {
  return editor_layout::body_length(g_app.editor.document.body);
}

uint16_t editor_line_start(uint16_t index) {
  if (index > editor_body_length()) {
    index = editor_body_length();
  }
  while (index > 0 && g_app.editor.document.body[index - 1] != '\n') {
    --index;
  }
  return index;
}

uint16_t editor_line_end(uint16_t index) {
  uint16_t len = editor_body_length();
  if (index > len) {
    index = len;
  }
  while (index < len && g_app.editor.document.body[index] != '\n') {
    ++index;
  }
  return index;
}

editor_layout::Position editor_cursor_position() {
  return editor_layout::position_from_index(
      g_app.editor.document.body, g_app.editor.cursor_index, EDITOR_VISIBLE_COLUMNS);
}

uint16_t editor_logical_line_count() {
  return editor_layout::logical_line_count(g_app.editor.document.body);
}

void editor_sync_preferred_visual_col() {
  g_app.editor.preferred_visual_col = editor_cursor_position().visual_col;
}

void editor_close_overlay() {
  g_app.editor.overlay = EDITOR_OVERLAY_NONE;
  g_app.editor.command_selected_index = 0;
}

const char* editor_command_label(editor_command_t command) {
  switch (command) {
    case EDITOR_COMMAND_SEARCH: return TR("Recherche", "Search");
    case EDITOR_COMMAND_GOTO_LINE: return TR("Aller ligne", "Go to line");
    case EDITOR_COMMAND_QUIT: return TR("Quitter", "Quit");
    case EDITOR_COMMAND_REDRAW: return TR("Redessiner", "Redraw");
  }
  return "";
}

void editor_ensure_cursor_visible() {
  const editor_layout::Position position = editor_cursor_position();

  if (position.visual_row < g_app.editor.top_visual_row) {
    g_app.editor.top_visual_row = position.visual_row;
  } else if (position.visual_row >= g_app.editor.top_visual_row + EDITOR_VISIBLE_LINES) {
    g_app.editor.top_visual_row =
        static_cast<uint16_t>(position.visual_row - EDITOR_VISIBLE_LINES + 1);
  }
}

void editor_request_text_command(text_input_target_t target,
                                 const char* title,
                                 const char* prompt,
                                 const char* initial_value) {
  editor_close_overlay();
  request_text_input(target, SCREEN_EDITOR, title, prompt, initial_value, false);
}

void editor_open_command_menu() {
  g_app.editor.overlay = EDITOR_OVERLAY_COMMAND_MENU;
  g_app.editor.command_selected_index = 0;
  g_app.full_redraw_needed = true;
}

// Back to where the document was opened from (explorer, project, tasks, search, home).
void editor_leave() {
  editor_close_overlay();
  g_app.editor.active = false;
  const screen_id_t target = g_app.editor.return_screen;
  switch_screen(target);
  if (target == SCREEN_PROJECT) {
    project_screen_resume();
  } else if (target == SCREEN_TASKS) {
    tasks_screen_resume();
  } else if (target == SCREEN_FILES && g_app.explorer.stale) {
    refresh_explorer(nullptr);
  }
}

void editor_request_exit() {
  if (g_app.editor.dirty) {
    g_app.editor.overlay = EDITOR_OVERLAY_CONFIRM_EXIT;
    g_app.full_redraw_needed = true;
    return;
  }

  editor_leave();
}

void editor_redraw_screen() {
  editor_close_overlay();
  minitel_ui_force_full_refresh();
  g_app.full_redraw_needed = true;
  set_status(TR("Ecran redessine", "Screen redrawn"));
}

bool parse_positive_line_number(const char* text, uint16_t* out_line_number) {
  if (!text || text[0] == '\0' || !out_line_number) {
    return false;
  }

  uint32_t value = 0;
  for (size_t i = 0; text[i] != '\0'; ++i) {
    if (text[i] < '0' || text[i] > '9') {
      return false;
    }
    value = value * 10 + static_cast<uint32_t>(text[i] - '0');
    if (value > 65535U) {
      return false;
    }
  }

  if (value == 0) {
    return false;
  }

  *out_line_number = static_cast<uint16_t>(value);
  return true;
}

void editor_apply_search_query(const char* query) {
  switch_screen(SCREEN_EDITOR);
  if (!query || query[0] == '\0') {
    set_status(TR("Recherche vide", "Empty search"));
    return;
  }

  safe_copy(g_app.editor.last_search, sizeof(g_app.editor.last_search), query);
  uint16_t found_index = 0;
  const uint16_t start_index =
      g_app.editor.cursor_index < editor_body_length()
          ? static_cast<uint16_t>(g_app.editor.cursor_index + 1)
          : static_cast<uint16_t>(editor_body_length());
  if (!editor_layout::find_forward(
          g_app.editor.document.body, g_app.editor.last_search, start_index, &found_index)) {
    set_status(TR("Texte introuvable", "Text not found"));
    return;
  }

  g_app.editor.cursor_index = found_index;
  editor_sync_preferred_visual_col();
  editor_ensure_cursor_visible();
  set_status(TR("Recherche trouvee", "Found"));
}

void editor_apply_goto_line(const char* text) {
  switch_screen(SCREEN_EDITOR);

  uint16_t requested_line = 0;
  if (!parse_positive_line_number(text, &requested_line)) {
    set_status(TR("Ligne invalide", "Invalid line"));
    return;
  }

  uint16_t line_count = editor_logical_line_count();
  if (requested_line > line_count) {
    requested_line = line_count;
  }

  g_app.editor.cursor_index =
      editor_layout::index_from_logical_line(g_app.editor.document.body,
                                             static_cast<uint16_t>(requested_line - 1));
  editor_sync_preferred_visual_col();
  editor_ensure_cursor_visible();

  char status[41];
  snprintf(status, sizeof(status), TR("Ligne %u", "Line %u"), static_cast<unsigned>(requested_line));
  set_status(status);
}

void editor_execute_command(editor_command_t command) {
  switch (command) {
    case EDITOR_COMMAND_SEARCH:
      editor_request_text_command(
          TEXT_INPUT_SEARCH_QUERY, TR("Recherche", "Search"), TR("Texte a trouver", "Text to find"),
          g_app.editor.last_search);
      return;
    case EDITOR_COMMAND_GOTO_LINE: {
      char line_value[8];
      const editor_layout::Position position = editor_cursor_position();
      snprintf(line_value, sizeof(line_value), "%u", static_cast<unsigned>(position.logical_line + 1));
      editor_request_text_command(TEXT_INPUT_GOTO_LINE, TR("Aller ligne", "Go to line"),
                                  TR("Numero de ligne", "Line number"), line_value);
      return;
    }
    case EDITOR_COMMAND_QUIT:
      editor_close_overlay();
      editor_request_exit();
      return;
    case EDITOR_COMMAND_REDRAW:
      editor_redraw_screen();
      return;
  }
}

void render_editor_command_menu() {
  for (uint8_t i = 0; i < EDITOR_COMMAND_COUNT; ++i) {
    char line[41];
    snprintf(line,
             sizeof(line),
             "%c %s",
             i == g_app.editor.command_selected_index ? '>' : ' ',
             editor_command_label(static_cast<editor_command_t>(i)));
    minitel_ui_write_line(static_cast<uint8_t>(EDITOR_MENU_START_ROW + i), line);
  }
}

void render_editor_confirm_exit() {
  minitel_ui_write_line(16, TR("Quitter sans sauver ?", "Quit without saving?"));
  minitel_ui_write_line(17, g_app.editor.document.title);
  minitel_ui_write_line(18, TR("ENVOI oui RETOUR/ANN non", "ENVOI yes RETOUR/ANN no"));
}

void render_editor_screen() {
  const char* footer = "ANN cmd RET quit CONN save";
  if (g_app.editor.overlay == EDITOR_OVERLAY_COMMAND_MENU) {
    footer = "REP/SUITE nav ENVOI ok ANN/RET";
  } else if (g_app.editor.overlay == EDITOR_OVERLAY_CONFIRM_EXIT) {
    footer = TR("ENVOI oui RETOUR/ANN non", "ENVOI yes RETOUR/ANN no");
  }
  render_frame_with_current_topbar("EDITOR", footer);
  editor_ensure_cursor_visible();

  char title_line[41];
  const char* title_suffix = g_app.editor.dirty ? " *" : "";
  if (g_app.editor.document.truncated) {
    title_suffix = TR(" [lecture seule]", " [read-only]");
  }
  snprintf(title_line,
           sizeof(title_line),
           "%s.%s%s",
           g_app.editor.document.title,
           g_app.editor.document.extension,
           title_suffix);
  minitel_ui_write_line(3, title_line);

  for (uint8_t row = 0; row < EDITOR_VISIBLE_LINES; ++row) {
    char line[EDITOR_VISIBLE_COLUMNS + 1];
    editor_layout::fill_visual_line(g_app.editor.document.body,
                                    static_cast<uint16_t>(g_app.editor.top_visual_row + row),
                                    EDITOR_VISIBLE_COLUMNS,
                                    line,
                                    sizeof(line));
    minitel_ui_write_line(static_cast<uint8_t>(EDITOR_BODY_START_ROW + row), line);
  }

  if (g_app.editor.overlay == EDITOR_OVERLAY_COMMAND_MENU) {
    render_editor_command_menu();
  } else if (g_app.editor.overlay == EDITOR_OVERLAY_CONFIRM_EXIT) {
    render_editor_confirm_exit();
  }

  const editor_layout::Position position = editor_cursor_position();
  char status[41];
  const char* line_text = status_line_text();
  if (g_app.shown_busy[0] != '\0') {
    safe_copy(status, sizeof(status), line_text);  // saving: the timer replaces Ln/Col
  } else if (g_app.editor.overlay == EDITOR_OVERLAY_COMMAND_MENU) {
    snprintf(status, sizeof(status), TR("Menu commandes", "Command menu"));
  } else if (g_app.editor.overlay == EDITOR_OVERLAY_CONFIRM_EXIT) {
    snprintf(status, sizeof(status), TR("Document modifie", "Document changed"));
  } else if (g_app.status_message[0]) {
    snprintf(status,
             sizeof(status),
             "Ln %u Col %u %s",
             static_cast<unsigned>(position.logical_line + 1),
             static_cast<unsigned>(position.logical_col + 1),
             g_app.status_message);
  } else {
    snprintf(status,
             sizeof(status),
             "Ln %u Col %u%s",
             static_cast<unsigned>(position.logical_line + 1),
             static_cast<unsigned>(position.logical_col + 1),
             g_app.editor.dirty ? " *" : "");
  }
  minitel_ui_write_line(STATUS_ROW, status);
}

// `until_input`: stops early when a key is waiting; the loop handles it, then draws the rest.
void render_current_screen(bool until_input) {
  switch (g_app.screen) {
    case SCREEN_HOME: render_home_screen(); break;
    case SCREEN_FILES: render_files_screen(); break;
    case SCREEN_WIFI: render_wifi_screen(); break;
    case SCREEN_SETTINGS: render_settings_screen(); break;
    case SCREEN_CHAT: render_chat_screen(); break;
    case SCREEN_TEXT_INPUT: render_text_input_screen(); break;
    case SCREEN_EDITOR: render_editor_screen(); break;
    case SCREEN_MODEL_PICKER: model_picker_render(); break;
    case SCREEN_SYSTEM: system_screen_render(); break;
    case SCREEN_PROJECTS: projects_screen_render(); break;
    case SCREEN_PROJECT: project_screen_render(); break;
    case SCREEN_TASKS: tasks_screen_render(); break;
    case SCREEN_SEARCH: search_screen_render(); break;
    case SCREEN_LOCK: lock_screen_render(); break;
    case SCREEN_BOOT: render_frame_with_current_topbar("BOOT", ""); break;
  }

  // Changing screen clears the page (FF) as at boot, instead of diffing against the previous
  // screen: nothing of it can stay behind.
  if (g_app.screen != g_app.rendered_screen) {
    minitel_ui_force_full_refresh();
    g_app.rendered_screen = g_app.screen;
  }

  if (!until_input) {
    minitel_ui_flush();
  } else if (!minitel_ui_flush_until_input()) {
    return;  // full_redraw_needed stays set
  }

  if (g_app.screen == SCREEN_TEXT_INPUT) {
    uint16_t start = 0;
    if (g_app.text_input.cursor > TEXT_INPUT_VISIBLE_COLUMNS - 2) {
      start = static_cast<uint16_t>(g_app.text_input.cursor - (TEXT_INPUT_VISIBLE_COLUMNS - 2));
    }
    uint8_t cursor_col = (g_app.text_input.cursor >= start)
                             ? static_cast<uint8_t>(g_app.text_input.cursor - start + 1)
                             : 1;
    minitel_ui_set_cursor_visible(true);
    minitel_ui_set_cursor(TEXT_INPUT_BUFFER_ROW, cursor_col);
  } else if (g_app.screen == SCREEN_EDITOR) {
    const editor_layout::Position position = editor_cursor_position();
    if (g_app.editor.overlay == EDITOR_OVERLAY_NONE &&
        position.visual_row >= g_app.editor.top_visual_row &&
        position.visual_row < g_app.editor.top_visual_row + EDITOR_VISIBLE_LINES) {
      minitel_ui_set_cursor_visible(true);
      minitel_ui_set_cursor(
          static_cast<uint8_t>(EDITOR_BODY_START_ROW + (position.visual_row - g_app.editor.top_visual_row)),
          static_cast<uint8_t>(position.visual_col + 1));
    } else {
      minitel_ui_set_cursor_visible(false);
    }
  } else {
    minitel_ui_set_cursor_visible(false);
  }

  build_topbar_line(g_app.last_topbar, sizeof(g_app.last_topbar));
  g_app.full_redraw_needed = false;
}

void switch_screen(screen_id_t screen) {
  app_log_info("nav", "switch_screen from=%s to=%s", screen_label(g_app.screen), screen_label(screen));
  g_app.screen = screen;
  g_app.full_redraw_needed = true;
  clear_status();
  net::screen_changed(screen);  // a request of the screen left is cancelled
}

void on_wifi_connected(const net_request_t& request, const net_reply_t& reply) {
  const bool open = request.params.wifi.password[0] == '\0';
  if (!reply.ok) {
    set_status(open ? TR("Connexion ouverte KO", "Open network failed")
                    : TR("Connexion WiFi KO", "WiFi connection failed"));
    return;
  }
  // Remembered here: the network task never writes NVS.
  safe_copy(g_settings.wifi_ssid, sizeof(g_settings.wifi_ssid), request.params.wifi.ssid);
  safe_copy(g_settings.wifi_password, sizeof(g_settings.wifi_password), request.params.wifi.password);
  g_settings_service.save(g_settings);
  check_store(open ? TR("Connexion ouverte OK", "Open network connected")
                   : TR("Connexion WiFi OK", "WiFi connected"));
}

// Connects to `ssid` and saves it as the default network. A failed or cancelled connect
// goes back to the saved network (when it is another one) instead of leaving the radio
// retrying this one; the link watch in app_loop then checks the server again.
void connect_wifi(const char* ssid, const char* password) {
  net_request_t request =
      net::make(NET_WIFI_CONNECT, TR("Connexion WiFi...", "Connecting WiFi..."), on_wifi_connected);
  auto& wifi = request.params.wifi;
  safe_copy(wifi.ssid, sizeof(wifi.ssid), ssid);
  safe_copy(wifi.password, sizeof(wifi.password), password);
  safe_copy(wifi.saved_ssid, sizeof(wifi.saved_ssid), g_settings.wifi_ssid);
  safe_copy(wifi.saved_password, sizeof(wifi.saved_password), g_settings.wifi_password);
  net::submit(request);
}

void on_saved_wifi_connected(const net_request_t& request, const net_reply_t& reply) {
  (void)request;
  app_log_info("boot", "wifi state=%s", reply.ok ? "connected" : "offline");
  if (!reply.ok) {
    check_store(nullptr);  // no link to wait for: the screens say "WiFi hors ligne" now
  }
}

// Boot: joins the saved network in the background, so that the home screen shows at once.
// Once the link is up, the link watch in app_loop starts NTP, network updates and the server
// check; when the network is away, the radio goes on trying it. False when nothing was
// submitted: no saved network, or no network task.
bool connect_saved_wifi() {
  if (g_settings.wifi_ssid[0] == '\0') {
    app_log_warn("wifi", "no saved ssid to connect");
    return false;
  }
  net_request_t request = net::make(NET_WIFI_CONNECT, nullptr, on_saved_wifi_connected);
  request.background = true;  // silent, and no screen change cancels it
  auto& wifi = request.params.wifi;
  safe_copy(wifi.ssid, sizeof(wifi.ssid), g_settings.wifi_ssid);
  safe_copy(wifi.password, sizeof(wifi.password), g_settings.wifi_password);
  // Asking for the saved network itself: a failure leaves the radio retrying it.
  safe_copy(wifi.saved_ssid, sizeof(wifi.saved_ssid), g_settings.wifi_ssid);
  safe_copy(wifi.saved_password, sizeof(wifi.saved_password), g_settings.wifi_password);
  return net::submit(request);
}

bool text_input_is_blank() {
  for (uint16_t i = 0; i < g_app.text_input.length; ++i) {
    const char ch = g_app.text_input.buffer[i];
    if (!isspace(static_cast<unsigned char>(ch)) && ch != '/' && ch != '\\') {
      return false;
    }
  }
  return true;
}

void on_wifi_scanned(const net_request_t& request, const net_reply_t& reply) {
  (void)request;
  g_app.wifi.network_count = static_cast<uint8_t>(reply.count < NET_MAX_NETWORKS ? reply.count : NET_MAX_NETWORKS);
  memcpy(g_app.wifi.networks, reply.data.networks, sizeof(g_app.wifi.networks[0]) * g_app.wifi.network_count);
  g_app.wifi.selected_index = 0;
  g_app.wifi.scroll_offset = 0;
  app_log_info("wifi", "scan_wifi_networks count=%u", static_cast<unsigned>(g_app.wifi.network_count));
  set_status(g_app.wifi.network_count > 0 ? TR("Scan termine", "Scan done") : TR("Aucun reseau", "No network"));
}

// The list on screen stays until the new one arrives.
void scan_wifi_networks() {
  net_request_t request = net::make(NET_WIFI_SCAN, TR("Scan WiFi...", "WiFi scan..."), on_wifi_scanned);
  net::submit(request);
}

void editor_insert_char(char ch) {
  int length = editor_body_length();
  if (length >= STORE_MAX_BODY_LENGTH) {
    set_status(TR("Document plein", "Document full"));
    return;
  }

  ch = invert_alpha_case_char(ch);
  memmove(g_app.editor.document.body + g_app.editor.cursor_index + 1,
          g_app.editor.document.body + g_app.editor.cursor_index,
          length - g_app.editor.cursor_index + 1);
  g_app.editor.document.body[g_app.editor.cursor_index++] = ch;
  g_app.editor.dirty = true;
  ++g_app.editor.revision;
  editor_sync_preferred_visual_col();
  g_app.full_redraw_needed = true;
}

void editor_insert_newline() {
  editor_insert_char('\n');
}

void editor_backspace() {
  if (g_app.editor.cursor_index == 0) {
    return;
  }

  int length = editor_body_length();
  memmove(g_app.editor.document.body + g_app.editor.cursor_index - 1,
          g_app.editor.document.body + g_app.editor.cursor_index,
          length - g_app.editor.cursor_index + 1);
  --g_app.editor.cursor_index;
  g_app.editor.dirty = true;
  ++g_app.editor.revision;
  editor_sync_preferred_visual_col();
  g_app.full_redraw_needed = true;
}

void editor_move_left() {
  if (g_app.editor.cursor_index > 0) {
    --g_app.editor.cursor_index;
    editor_sync_preferred_visual_col();
    g_app.full_redraw_needed = true;
  }
}

void editor_move_right() {
  if (g_app.editor.cursor_index < editor_body_length()) {
    ++g_app.editor.cursor_index;
    editor_sync_preferred_visual_col();
    g_app.full_redraw_needed = true;
  }
}

void editor_move_up() {
  const editor_layout::Position position = editor_cursor_position();
  if (position.visual_row == 0) {
    return;
  }

  g_app.editor.cursor_index = editor_layout::index_from_visual(g_app.editor.document.body,
                                                               static_cast<uint16_t>(position.visual_row - 1),
                                                               g_app.editor.preferred_visual_col,
                                                               EDITOR_VISIBLE_COLUMNS);
  g_app.full_redraw_needed = true;
}

void editor_move_down() {
  const editor_layout::Position position = editor_cursor_position();
  const uint16_t total_rows = editor_layout::visual_row_count(g_app.editor.document.body, EDITOR_VISIBLE_COLUMNS);
  if (position.visual_row + 1 >= total_rows) {
    return;
  }

  g_app.editor.cursor_index = editor_layout::index_from_visual(g_app.editor.document.body,
                                                               static_cast<uint16_t>(position.visual_row + 1),
                                                               g_app.editor.preferred_visual_col,
                                                               EDITOR_VISIBLE_COLUMNS);
  g_app.full_redraw_needed = true;
}

void editor_move_home() {
  g_app.editor.cursor_index = editor_line_start(g_app.editor.cursor_index);
  editor_sync_preferred_visual_col();
  g_app.full_redraw_needed = true;
}

void editor_move_end() {
  g_app.editor.cursor_index = editor_line_end(g_app.editor.cursor_index);
  editor_sync_preferred_visual_col();
  g_app.full_redraw_needed = true;
}

void editor_page_move(int delta) {
  const editor_layout::Position position = editor_cursor_position();
  const uint16_t total_rows = editor_layout::visual_row_count(g_app.editor.document.body, EDITOR_VISIBLE_COLUMNS);
  int target = static_cast<int>(position.visual_row) + delta;
  if (target < 0) {
    target = 0;
  } else if (target >= total_rows) {
    target = total_rows - 1;
  }
  g_app.editor.cursor_index = editor_layout::index_from_visual(g_app.editor.document.body,
                                                               static_cast<uint16_t>(target),
                                                               g_app.editor.preferred_visual_col,
                                                               EDITOR_VISIBLE_COLUMNS);
  g_app.full_redraw_needed = true;
}

void on_document_created(const net_request_t& request, const net_reply_t& reply) {
  (void)request;
  if (!reply.ok) {
    set_status_from_reply_or(reply, TR("Creation document KO", "Document not created"));
    return;
  }
  g_app.explorer.stale = true;  // listed again when the editor closes
  show_document(reply.data.document, 0, SCREEN_FILES);
  set_status(TR("Document cree", "Document created"));
}

void on_folder_created(const net_request_t& request, const net_reply_t& reply) {
  (void)request;
  if (!reply.ok) {
    set_status_from_reply_or(reply, TR("Creation dossier KO", "Folder not created"));
    return;
  }
  refresh_explorer(TR("Dossier cree", "Folder created"));
}

void on_tags_updated(const net_request_t& request, const net_reply_t& reply) {
  (void)request;
  if (!reply.ok) {
    set_status_from_reply_or(reply, TR("Maj tags KO", "Tags not updated"));
    return;
  }
  refresh_explorer(TR("Tags mis a jour", "Tags updated"));
}

void apply_text_input() {
  const bool masked = g_app.text_input.masked;
  app_log_info("input",
               "apply target=%s len=%u masked=%s",
               text_input_target_label(g_app.text_input.target),
               static_cast<unsigned>(g_app.text_input.length),
               masked ? "yes" : "no");
  switch (g_app.text_input.target) {
    case TEXT_INPUT_WIFI_PASSWORD:
      switch_screen(SCREEN_WIFI);
      connect_wifi(g_app.wifi.pending_ssid, g_app.text_input.buffer);
      break;

    // Requests read the settings when they start: nothing else to reconfigure.
    case TEXT_INPUT_API_URL:
      safe_copy(g_settings.api_url, sizeof(g_settings.api_url), g_app.text_input.buffer);
      g_settings_service.save(g_settings);
      switch_screen(SCREEN_SETTINGS);
      check_store(TR("Docs URL enregistree", "Docs URL saved"));
      break;

    case TEXT_INPUT_API_TOKEN:
      safe_copy(g_settings.api_token, sizeof(g_settings.api_token), g_app.text_input.buffer);
      g_settings_service.save(g_settings);
      if (g_wifi_service.is_connected()) {
        start_lan_updates();  // the token is also the network update password
      }
      switch_screen(SCREEN_SETTINGS);
      check_store(TR("Docs token enregistre", "Docs token saved"));
      break;

    case TEXT_INPUT_AI_BASE_URL:
      safe_copy(g_settings.ai_base_url, sizeof(g_settings.ai_base_url), g_app.text_input.buffer);
      g_settings_service.save(g_settings);
      switch_screen(SCREEN_SETTINGS);
      set_status(TR("IA URL enregistree", "AI URL saved"));
      break;

    case TEXT_INPUT_AI_API_KEY:
      safe_copy(g_settings.ai_api_key, sizeof(g_settings.ai_api_key), g_app.text_input.buffer);
      g_settings_service.save(g_settings);
      switch_screen(SCREEN_SETTINGS);
      set_status(TR("IA key enregistree", "AI key saved"));
      break;

    case TEXT_INPUT_TIMEZONE:
      safe_copy(g_settings.timezone, sizeof(g_settings.timezone), g_app.text_input.buffer);
      g_settings_service.save(g_settings);
      g_clock_service.set_timezone(g_settings.timezone);
      switch_screen(SCREEN_SETTINGS);
      set_status(TR("Timezone mise a jour", "Timezone updated"));
      break;

    case TEXT_INPUT_NEW_DOCUMENT: {
      switch_screen(SCREEN_FILES);
      if (text_input_is_blank()) {
        set_status(TR("Nom vide: rien cree", "Empty name: nothing created"));
        break;
      }
      net_request_t request =
          net::make(NET_CREATE_DOCUMENT, TR("Creation du document...", "Creating document..."), on_document_created);
      safe_copy(request.params.create.folder, sizeof(request.params.create.folder), g_app.explorer.current_folder);
      safe_copy(request.params.create.name, sizeof(request.params.create.name), g_app.text_input.buffer);
      net::submit(request);
      break;
    }

    case TEXT_INPUT_NEW_FOLDER: {
      switch_screen(SCREEN_FILES);
      if (text_input_is_blank()) {
        // An empty name would resolve to the current folder and re-create it.
        set_status(TR("Nom vide: rien cree", "Empty name: nothing created"));
        break;
      }
      net_request_t request =
          net::make(NET_CREATE_FOLDER, TR("Creation du dossier...", "Creating folder..."), on_folder_created);
      safe_copy(request.params.create.folder, sizeof(request.params.create.folder), g_app.explorer.current_folder);
      safe_copy(request.params.create.name, sizeof(request.params.create.name), g_app.text_input.buffer);
      net::submit(request);
      break;
    }

    case TEXT_INPUT_EDIT_TAGS: {
      switch_screen(SCREEN_FILES);
      net_request_t request =
          net::make(NET_UPDATE_TAGS, TR("Mise a jour des tags...", "Updating tags..."), on_tags_updated);
      safe_copy(request.params.tags.id,
                sizeof(request.params.tags.id),
                g_app.explorer.items[g_app.explorer.selected_index].document_id);
      safe_copy(request.params.tags.csv, sizeof(request.params.tags.csv), g_app.text_input.buffer);
      net::submit(request);
      break;
    }

    case TEXT_INPUT_FILTER_TAG:
      switch_screen(SCREEN_FILES);
      request_explorer_listing(g_app.explorer.current_folder,
                               g_app.text_input.buffer,
                               nullptr,
                               g_app.text_input.buffer[0] ? TR("Filtre active", "Filter on")
                                                          : TR("Filtre retire", "Filter off"));
      break;

    case TEXT_INPUT_SEARCH_QUERY:
      editor_apply_search_query(g_app.text_input.buffer);
      break;

    case TEXT_INPUT_GOTO_LINE:
      editor_apply_goto_line(g_app.text_input.buffer);
      break;

    case TEXT_INPUT_CHAT_PROMPT:
      chat_submit_prompt(g_app.text_input.buffer);
      break;

    case TEXT_INPUT_CALLBACK: {
      // Copy first: the callback may open another text input that reuses the buffer.
      char value[sizeof(g_app.text_input.buffer)];
      safe_copy(value, sizeof(value), g_app.text_input.buffer);
      const text_input_done_t on_done = g_app.text_input.on_done;
      switch_screen(g_app.text_input.return_screen);
      if (on_done) {
        on_done(value);
      }
      break;
    }

    case TEXT_INPUT_NONE:
      switch_screen(g_app.text_input.return_screen);
      break;
  }
}

void on_captured(const net_request_t& request, const net_reply_t& reply) {
  (void)request;
  if (reply.ok) {
    set_status(TR("Note ajoutee a inbox/inbox.md", "Note added to inbox/inbox.md"));
  } else {
    set_status_from_reply_or(reply, TR("Capture impossible", "Capture failed"));
  }
}

void on_capture(const char* text) {
  if (!text || text[0] == '\0') {
    set_status(TR("Capture vide: rien note", "Empty capture: nothing saved"));
    return;
  }
  char line[CHAT_MAX_PROMPT_LENGTH + 24];
  struct tm now;
  if (g_clock_service.local_time(&now)) {
    char stamp[20];
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M", &now);
    snprintf(line, sizeof(line), "- %s %s", stamp, text);
  } else {
    snprintf(line, sizeof(line), "- %s", text);
  }
  net_request_t request = net::make(NET_APPEND_LINE, TR("Capture...", "Saving note..."), on_captured);
  safe_copy(request.params.append.id, sizeof(request.params.append.id), INBOX_FILE);
  safe_copy(request.params.append.text, sizeof(request.params.append.text), line);
  net::submit(request);
}

void on_journal_heading_added(const net_request_t& request, const net_reply_t& reply) {
  if (!reply.ok) {
    set_status_from_reply_or(reply, TR("Journal indisponible", "Journal unavailable"));
    return;
  }
  char local_id[sizeof(request.params.append.id) + 1];
  snprintf(local_id, sizeof(local_id), "/%s", request.params.append.id);
  request_document(local_id, UINT16_MAX, SCREEN_HOME);
}

// Today's journal: each opening adds a "## HH:MM" heading and puts the cursor below it.
void open_journal() {
  struct tm now;
  if (!g_clock_service.local_time(&now)) {
    set_status(TR("Journal: heure pas synchronisee", "Journal: clock not synced"));
    return;
  }
  char date[11];
  char heading[12];
  strftime(date, sizeof(date), "%Y-%m-%d", &now);
  strftime(heading, sizeof(heading), "## %H:%M", &now);
  net_request_t request = net::make(NET_APPEND_LINE, "Journal...", on_journal_heading_added);
  snprintf(request.params.append.id, sizeof(request.params.append.id), "journal/%s.md", date);
  safe_copy(request.params.append.text, sizeof(request.params.append.text), heading);
  net::submit(request);
}

void open_home_item(uint8_t index) {
  app_log_info("nav", "home open item=%u", static_cast<unsigned>(index));
  switch (index) {
    case 0:
      projects_screen_open();
      return;
    case 1:
      tasks_screen_open();
      return;
    case 2:
      switch_screen(SCREEN_FILES);
      refresh_explorer(nullptr);
      return;
    case 3:
      shell::request_text_input(SCREEN_HOME, TR("Capture rapide", "Quick capture"),
                                TR("Idee, note, rappel...", "Idea, note, reminder..."), "", false, on_capture);
      return;
    case 4:
      open_journal();
      return;
    case 5:
      search_screen_start();
      return;
    case 6:
      switch_screen(SCREEN_CHAT);
      return;
    case 7:
      switch_screen(SCREEN_SETTINGS);
      return;
    case 8:
      system_screen_open();
      return;
    default:
      break;
  }
  const uint8_t recent = static_cast<uint8_t>(index - HOME_MENU_COUNT);
  if (recent < workspace::recent_count()) {
    request_document(workspace::recent_at(recent).id, 0, SCREEN_HOME);
  }
}

void handle_home_input(const input_event_t& event) {
  const uint8_t item_count = static_cast<uint8_t>(HOME_MENU_COUNT + workspace::recent_count());
  if (event.type == INPUT_EVENT_UP && g_app.home_selected_index > 0) {
    --g_app.home_selected_index;
    g_app.full_redraw_needed = true;
    return;
  }
  if (event.type == INPUT_EVENT_DOWN && g_app.home_selected_index + 1 < item_count) {
    ++g_app.home_selected_index;
    g_app.full_redraw_needed = true;
    return;
  }
  if (event.type == INPUT_EVENT_PRINTABLE && event.ch >= '1' && event.ch <= '9') {
    g_app.home_selected_index = static_cast<uint8_t>(event.ch - '1');
    g_app.full_redraw_needed = true;
    open_home_item(g_app.home_selected_index);
    return;
  }
  if (event.type == INPUT_EVENT_ENTER) {
    open_home_item(g_app.home_selected_index);
  }
}

void handle_files_input(const input_event_t& event) {
  if (event.type == INPUT_EVENT_UP && g_app.explorer.selected_index > 0) {
    --g_app.explorer.selected_index;
    ensure_explorer_selection_visible();
    g_app.full_redraw_needed = true;
    app_log_info("files",
                 "selection index=%u label=\"%s\"",
                 static_cast<unsigned>(g_app.explorer.selected_index),
                 g_app.explorer.items[g_app.explorer.selected_index].label);
    return;
  }
  if (event.type == INPUT_EVENT_DOWN && g_app.explorer.selected_index + 1 < g_app.explorer.item_count) {
    ++g_app.explorer.selected_index;
    ensure_explorer_selection_visible();
    g_app.full_redraw_needed = true;
    app_log_info("files",
                 "selection index=%u label=\"%s\"",
                 static_cast<unsigned>(g_app.explorer.selected_index),
                 g_app.explorer.items[g_app.explorer.selected_index].label);
    return;
  }
  if (event.type == INPUT_EVENT_PAGE_UP) {
    if (g_app.explorer.selected_index > 5) {
      g_app.explorer.selected_index -= 5;
    } else {
      g_app.explorer.selected_index = 0;
    }
    ensure_explorer_selection_visible();
    g_app.full_redraw_needed = true;
    if (g_app.explorer.item_count > 0) {
      app_log_info("files",
                   "page_up selection index=%u label=\"%s\"",
                   static_cast<unsigned>(g_app.explorer.selected_index),
                   g_app.explorer.items[g_app.explorer.selected_index].label);
    }
    return;
  }
  if (event.type == INPUT_EVENT_PAGE_DOWN && g_app.explorer.item_count > 0) {
    g_app.explorer.selected_index =
        min(static_cast<int>(g_app.explorer.item_count - 1), static_cast<int>(g_app.explorer.selected_index + 5));
    ensure_explorer_selection_visible();
    g_app.full_redraw_needed = true;
    app_log_info("files",
                 "page_down selection index=%u label=\"%s\"",
                 static_cast<unsigned>(g_app.explorer.selected_index),
                 g_app.explorer.items[g_app.explorer.selected_index].label);
    return;
  }
  if (event.type == INPUT_EVENT_ESCAPE) {
    if (strcmp(g_app.explorer.current_folder, "/") == 0) {
      app_log_info("files", "escape to HOME");
      switch_screen(SCREEN_HOME);
    } else {
      // The explorer moves to the parent once it is listed, with this folder selected.
      char parent_folder[sizeof(g_app.explorer.current_folder)];
      safe_copy(parent_folder, sizeof(parent_folder), g_app.explorer.current_folder);
      char* slash = strrchr(parent_folder, '/');
      if (slash && slash != parent_folder) {
        *slash = '\0';
      } else {
        safe_copy(parent_folder, sizeof(parent_folder), "/");
      }
      app_log_info("files", "escape to parent folder=\"%s\"", parent_folder);
      request_explorer_listing(parent_folder, g_app.explorer.tag_filter, g_app.explorer.current_folder, nullptr);
    }
    return;
  }
  if (event.type == INPUT_EVENT_CANCEL) {
    request_explorer_listing(g_app.explorer.current_folder, "", nullptr, TR("Filtre retire", "Filter off"));
    return;
  }

  if (event.type == INPUT_EVENT_PRINTABLE) {
    switch (event.ch) {
      case 'n':
      case 'N':
        if (strcmp(g_app.explorer.current_folder, "/") == 0) {
          set_status(TR("Racine virtuelle: cree un dossier", "Virtual root: create a folder"));
          return;
        }
        request_text_input(TEXT_INPUT_NEW_DOCUMENT,
                           SCREEN_FILES,
                           TR("Nouveau document", "New document"),
                           TR("Nom (.md par defaut)", "Name (.md by default)"),
                           "",
                           false);
        return;
      case 'd':
      case 'D':
        request_text_input(TEXT_INPUT_NEW_FOLDER, SCREEN_FILES, TR("Nouveau dossier", "New folder"),
                           TR("Nom du sous-dossier", "Subfolder name"), "", false);
        return;
      case 'f':
      case 'F':
        request_text_input(TEXT_INPUT_FILTER_TAG,
                           SCREEN_FILES,
                           TR("Filtre tag", "Tag filter"),
                           TR("Laisser vide pour tout", "Leave empty for all"),
                           g_app.explorer.tag_filter,
                           false);
        return;
      case 't':
      case 'T':
        if (g_app.explorer.item_count > 0 &&
            g_app.explorer.items[g_app.explorer.selected_index].kind == EXPLORER_ITEM_DOCUMENT) {
          request_text_input(TEXT_INPUT_EDIT_TAGS,
                             SCREEN_FILES,
                             TR("Tags document", "Document tags"),
                             TR("Liste separee par virgules", "Comma-separated list"),
                             g_app.explorer.items[g_app.explorer.selected_index].meta,
                             false);
        }
        return;
      default:
        break;
    }
  }

  if (event.type == INPUT_EVENT_ENTER && g_app.explorer.item_count > 0) {
    explorer_item_t& item = g_app.explorer.items[g_app.explorer.selected_index];
    if (item.kind == EXPLORER_ITEM_FOLDER) {
      app_log_info("files", "open folder path=\"%s\"", item.path);
      request_explorer_listing(item.path, g_app.explorer.tag_filter, nullptr, nullptr);
    } else {
      app_log_info("files", "open document id=\"%s\"", item.document_id);
      request_document(item.document_id, 0, SCREEN_FILES);
    }
  }
}

void ensure_wifi_selection_visible() {
  if (g_app.wifi.selected_index < g_app.wifi.scroll_offset) {
    g_app.wifi.scroll_offset = g_app.wifi.selected_index;
  } else if (g_app.wifi.selected_index >= g_app.wifi.scroll_offset + WIFI_VISIBLE_ROWS) {
    g_app.wifi.scroll_offset = g_app.wifi.selected_index - WIFI_VISIBLE_ROWS + 1;
  }
}

void handle_wifi_input(const input_event_t& event) {
  if (event.type == INPUT_EVENT_UP && g_app.wifi.selected_index > 0) {
    --g_app.wifi.selected_index;
    ensure_wifi_selection_visible();
    g_app.full_redraw_needed = true;
    app_log_info("wifi", "selection index=%u ssid=\"%s\"",
                 static_cast<unsigned>(g_app.wifi.selected_index),
                 g_app.wifi.networks[g_app.wifi.selected_index].ssid);
    return;
  }
  if (event.type == INPUT_EVENT_DOWN && g_app.wifi.selected_index + 1 < g_app.wifi.network_count) {
    ++g_app.wifi.selected_index;
    ensure_wifi_selection_visible();
    g_app.full_redraw_needed = true;
    app_log_info("wifi", "selection index=%u ssid=\"%s\"",
                 static_cast<unsigned>(g_app.wifi.selected_index),
                 g_app.wifi.networks[g_app.wifi.selected_index].ssid);
    return;
  }
  if (event.type == INPUT_EVENT_ESCAPE) {
    app_log_info("wifi", "escape to SETTINGS");
    switch_screen(SCREEN_SETTINGS);
    return;
  }
  if (event.type == INPUT_EVENT_PRINTABLE && (event.ch == 'r' || event.ch == 'R')) {
    scan_wifi_networks();
    return;
  }

  // Not while a scan or a connect runs: a password typed meanwhile would be refused.
  if (event.type == INPUT_EVENT_ENTER && g_app.wifi.network_count > 0 && !net::busy()) {
    const lm_wifi_network_t& network = g_app.wifi.networks[g_app.wifi.selected_index];
    app_log_info("wifi",
                 "activate ssid=\"%s\" encrypted=%s",
                 network.ssid,
                 network.encrypted ? "yes" : "no");
    safe_copy(g_app.wifi.pending_ssid, sizeof(g_app.wifi.pending_ssid), network.ssid);

    if (!network.encrypted) {
      connect_wifi(network.ssid, "");
      return;
    }

    const bool same_saved_ssid = strcmp(network.ssid, g_settings.wifi_ssid) == 0;
    if (same_saved_ssid && g_settings.wifi_password[0] != '\0') {
      connect_wifi(network.ssid, g_settings.wifi_password);
      return;
    }

    request_text_input(TEXT_INPUT_WIFI_PASSWORD,
                       SCREEN_WIFI,
                       TR("Mot de passe WiFi", "WiFi password"),
                       network.ssid,
                       "",
                       true);
  }
}

void handle_settings_input(const input_event_t& event) {
  const bool reset_confirmed = g_app.settings_view.reset_armed && event.type == INPUT_EVENT_ENTER &&
                               g_app.settings_view.selected_index == SETTINGS_RESET_INDEX;
  if (g_app.settings_view.reset_armed && !reset_confirmed) {
    // Any other key cancels the pending reset.
    clear_status();
    g_app.full_redraw_needed = true;
  }
  g_app.settings_view.reset_armed = false;

  if (event.type == INPUT_EVENT_UP && g_app.settings_view.selected_index > 0) {
    --g_app.settings_view.selected_index;
    g_app.full_redraw_needed = true;
    app_log_info("settings", "selection index=%u", static_cast<unsigned>(g_app.settings_view.selected_index));
    return;
  }
  if (event.type == INPUT_EVENT_DOWN && g_app.settings_view.selected_index + 1 < SETTINGS_ITEM_COUNT) {
    ++g_app.settings_view.selected_index;
    g_app.full_redraw_needed = true;
    app_log_info("settings", "selection index=%u", static_cast<unsigned>(g_app.settings_view.selected_index));
    return;
  }
  if (event.type == INPUT_EVENT_ESCAPE) {
    app_log_info("settings", "escape to HOME");
    switch_screen(SCREEN_HOME);
    return;
  }
  if (event.type != INPUT_EVENT_ENTER) {
    return;
  }

  switch (g_app.settings_view.selected_index) {
    case 0:
      switch_screen(SCREEN_WIFI);  // first: the scan belongs to the WIFI screen
      scan_wifi_networks();
      break;
    case 1:
      request_text_input(TEXT_INPUT_API_URL,
                         SCREEN_SETTINGS,
                         "Docs API URL",
                         TR("Base URL Bibliotek", "Bibliotek base URL"),
                         g_settings.api_url,
                         false);
      break;
    case 2:
      request_text_input(TEXT_INPUT_API_TOKEN,
                         SCREEN_SETTINGS,
                         "Docs token",
                         "Bearer token docs",
                         g_settings.api_token,
                         true);
      break;
    case 3:
      request_text_input(TEXT_INPUT_AI_BASE_URL,
                         SCREEN_SETTINGS,
                         TR("IA URL", "AI URL"),
                         TR("Base URL Gradient", "Gradient base URL"),
                         g_settings.ai_base_url,
                         false);
      break;
    case 4:
      request_text_input(TEXT_INPUT_AI_API_KEY,
                         SCREEN_SETTINGS,
                         TR("IA key", "AI key"),
                         TR("Bearer key Gradient", "Gradient bearer key"),
                         g_settings.ai_api_key,
                         true);
      break;
    case 5:
      model_picker_open();
      break;
    case SETTINGS_AI_TOOLS_INDEX:
      g_settings.ai_tools = !g_settings.ai_tools;
      g_settings_service.save(g_settings);
      set_status(g_settings.ai_tools ? TR("L'IA peut lire et ecrire tes notes", "The AI can read and write your notes")
                                     : TR("IA sans acces aux notes", "AI without access to notes"));
      break;
    case SETTINGS_TIMEZONE_INDEX:
      request_text_input(TEXT_INPUT_TIMEZONE,
                         SCREEN_SETTINGS,
                         "Timezone",
                         TR("Ex: Europe/Paris", "E.g. Europe/London"),
                         g_settings.timezone,
                         false);
      break;
    case SETTINGS_PIN_INDEX:
      shell::request_text_input(SCREEN_SETTINGS, TR("Code PIN", "PIN code"),
                                TR("4 a 8 chiffres, vide = aucun", "4 to 8 digits, empty = none"), "", true,
                                lock_screen_change_pin);
      break;
    case SETTINGS_RESET_INDEX:
      if (!reset_confirmed) {
        g_app.settings_view.reset_armed = true;
        set_status(TR("ENVOI encore pour tout effacer", "ENVOI again to erase everything"));
        break;
      }
      g_settings_service.reset();
      g_settings_service.load(&g_settings);
      g_clock_service.set_timezone(g_settings.timezone);
      check_store(TR("Settings reinitialises", "Settings reset"));
      g_app.full_redraw_needed = true;
      break;
  }
}

void handle_chat_input(const input_event_t& event) {
  chat_rebuild_render_lines();
  chat_ensure_scroll_in_bounds();

  if (event.type == INPUT_EVENT_UP && g_app.chat.scroll_offset > 0) {
    --g_app.chat.scroll_offset;
    g_app.chat.follow = false;
    g_app.full_redraw_needed = true;
    return;
  }
  if (event.type == INPUT_EVENT_DOWN &&
      g_app.chat.scroll_offset + CHAT_VISIBLE_ROWS < g_app.chat.total_lines) {
    ++g_app.chat.scroll_offset;
    g_app.chat.follow = g_app.chat.scroll_offset + CHAT_VISIBLE_ROWS >= g_app.chat.total_lines;
    g_app.full_redraw_needed = true;
    return;
  }
  if (event.type == INPUT_EVENT_PAGE_UP) {
    if (g_app.chat.scroll_offset > CHAT_VISIBLE_ROWS) {
      g_app.chat.scroll_offset = static_cast<uint16_t>(g_app.chat.scroll_offset - CHAT_VISIBLE_ROWS);
    } else {
      g_app.chat.scroll_offset = 0;
    }
    g_app.chat.follow = false;
    g_app.full_redraw_needed = true;
    return;
  }
  if (event.type == INPUT_EVENT_PAGE_DOWN) {
    const uint16_t bottom = chat_bottom_offset();
    if (g_app.chat.scroll_offset < bottom) {
      const uint32_t next_offset = static_cast<uint32_t>(g_app.chat.scroll_offset) + CHAT_VISIBLE_ROWS;
      g_app.chat.scroll_offset = static_cast<uint16_t>(next_offset > bottom ? bottom : next_offset);
    }
    g_app.chat.follow = g_app.chat.scroll_offset >= bottom;
    g_app.full_redraw_needed = true;
    return;
  }
  if (event.type == INPUT_EVENT_HOME) {
    g_app.chat.scroll_offset = 0;
    g_app.chat.follow = false;
    g_app.full_redraw_needed = true;
    return;
  }
  if (event.type == INPUT_EVENT_END) {
    chat_scroll_to_bottom();
    g_app.chat.follow = true;
    g_app.full_redraw_needed = true;
    return;
  }
  if (event.type == INPUT_EVENT_ESCAPE) {
    // A reply still streaming stops there, what arrived is kept. A confirmation still waits:
    // the user may check the note before answering.
    switch_screen(SCREEN_HOME);
    return;
  }
  if (g_app.chat.awaiting) {
    if (event.type == INPUT_EVENT_ENTER) {
      chat_answer_confirmation(true);
    } else if (event.type == INPUT_EVENT_BACKSPACE) {
      chat_answer_confirmation(false);
    } else if (event.type == INPUT_EVENT_CANCEL) {
      chat_abandon_confirmation();
    }
    return;
  }
  if (event.type == INPUT_EVENT_CANCEL) {
    chat_clear_session(true);  // while a reply streams, ANNULATION stops it instead
    return;
  }
  if (event.type == INPUT_EVENT_ENTER && !g_app.chat.pending) {
    chat_request_prompt_input();
  }
}

void handle_text_input(const input_event_t& event) {
  if (event.type == INPUT_EVENT_LEFT && g_app.text_input.cursor > 0) {
    --g_app.text_input.cursor;
    g_app.full_redraw_needed = true;
    return;
  }
  if (event.type == INPUT_EVENT_RIGHT && g_app.text_input.cursor < g_app.text_input.length) {
    ++g_app.text_input.cursor;
    g_app.full_redraw_needed = true;
    return;
  }
  if (event.type == INPUT_EVENT_HOME) {
    g_app.text_input.cursor = 0;
    g_app.full_redraw_needed = true;
    return;
  }
  if (event.type == INPUT_EVENT_END) {
    g_app.text_input.cursor = g_app.text_input.length;
    g_app.full_redraw_needed = true;
    return;
  }
  if (event.type == INPUT_EVENT_BACKSPACE && g_app.text_input.cursor > 0) {
    memmove(g_app.text_input.buffer + g_app.text_input.cursor - 1,
            g_app.text_input.buffer + g_app.text_input.cursor,
            g_app.text_input.length - g_app.text_input.cursor + 1);
    --g_app.text_input.cursor;
    --g_app.text_input.length;
    g_app.full_redraw_needed = true;
    return;
  }
  if ((event.type == INPUT_EVENT_ESCAPE || event.type == INPUT_EVENT_CANCEL)) {
    switch_screen(g_app.text_input.return_screen);
    return;
  }
  if (event.type == INPUT_EVENT_ENTER) {
    apply_text_input();
    return;
  }
  if (event.type == INPUT_EVENT_PRINTABLE && g_app.text_input.length + 1 < sizeof(g_app.text_input.buffer)) {
    const char input_ch =
        text_input_uses_inverted_alpha_case(g_app.text_input.target, g_app.text_input.masked)
            ? invert_alpha_case_char(event.ch)
            : event.ch;
    memmove(g_app.text_input.buffer + g_app.text_input.cursor + 1,
            g_app.text_input.buffer + g_app.text_input.cursor,
            g_app.text_input.length - g_app.text_input.cursor + 1);
    g_app.text_input.buffer[g_app.text_input.cursor] = input_ch;
    ++g_app.text_input.cursor;
    ++g_app.text_input.length;
    g_app.full_redraw_needed = true;
  }
}

// Network task input: the document as it is when the save starts.
void prepare_save(net_request_t& request, net_reply_t& reply) {
  reply.data.document = g_app.editor.document;
  request.params.save.revision = g_app.editor.revision;
}

void on_document_saved(const net_request_t& request, const net_reply_t& reply) {
  if (!reply.ok) {
    set_status_from_reply_or(reply, TR("Sauvegarde impossible", "Save failed"));
    return;
  }
  // Edits typed during the save are not on the server yet.
  if (g_app.editor.active && g_app.editor.revision == request.params.save.revision) {
    g_app.editor.dirty = false;
  }
  set_status(TR("Document sauvegarde", "Document saved"));
}

void handle_editor_input(const input_event_t& event) {
  if (g_app.editor.overlay == EDITOR_OVERLAY_COMMAND_MENU) {
    if (event.type == INPUT_EVENT_UP && g_app.editor.command_selected_index > 0) {
      --g_app.editor.command_selected_index;
      g_app.full_redraw_needed = true;
      return;
    }
    if (event.type == INPUT_EVENT_DOWN &&
        g_app.editor.command_selected_index + 1 < EDITOR_COMMAND_COUNT) {
      ++g_app.editor.command_selected_index;
      g_app.full_redraw_needed = true;
      return;
    }
    if (event.type == INPUT_EVENT_ENTER) {
      editor_execute_command(static_cast<editor_command_t>(g_app.editor.command_selected_index));
      return;
    }
    if (event.type == INPUT_EVENT_ESCAPE || event.type == INPUT_EVENT_CANCEL) {
      editor_close_overlay();
      g_app.full_redraw_needed = true;
      return;
    }
    if (event.type == INPUT_EVENT_PRINTABLE) {
      switch (event.ch) {
        case 'r':
        case 'R':
        case '/':
          editor_execute_command(EDITOR_COMMAND_SEARCH);
          return;
        case 'g':
        case 'G':
        case 'l':
        case 'L':
          editor_execute_command(EDITOR_COMMAND_GOTO_LINE);
          return;
        case 'q':
        case 'Q':
          editor_execute_command(EDITOR_COMMAND_QUIT);
          return;
        case 'd':
        case 'D':
          editor_execute_command(EDITOR_COMMAND_REDRAW);
          return;
        default:
          return;
      }
    }
    return;
  }

  if (g_app.editor.overlay == EDITOR_OVERLAY_CONFIRM_EXIT) {
    if (event.type == INPUT_EVENT_ENTER) {
      g_app.editor.dirty = false;
      editor_leave();
      set_status(TR("Edition abandonnee", "Changes discarded"));
      return;
    }
    if (event.type == INPUT_EVENT_ESCAPE || event.type == INPUT_EVENT_CANCEL) {
      editor_close_overlay();
      g_app.full_redraw_needed = true;
      return;
    }
    return;
  }

  if (g_app.editor.document.truncated) {
    switch (event.type) {
      case INPUT_EVENT_PRINTABLE:
      case INPUT_EVENT_ENTER:
      case INPUT_EVENT_BACKSPACE:
      case INPUT_EVENT_SAVE:
        set_status(TR("Lecture seule: doc trop long", "Read-only: document too long"));
        return;
      default:
        break;
    }
  }

  switch (event.type) {
    case INPUT_EVENT_PRINTABLE:
      editor_insert_char(event.ch);
      return;
    case INPUT_EVENT_ENTER:
      editor_insert_newline();
      return;
    case INPUT_EVENT_BACKSPACE:
      editor_backspace();
      return;
    case INPUT_EVENT_LEFT:
      editor_move_left();
      return;
    case INPUT_EVENT_RIGHT:
      editor_move_right();
      return;
    case INPUT_EVENT_UP:
      editor_move_up();
      return;
    case INPUT_EVENT_DOWN:
      editor_move_down();
      return;
    case INPUT_EVENT_HOME:
      editor_move_home();
      return;
    case INPUT_EVENT_END:
      editor_move_end();
      return;
    case INPUT_EVENT_PAGE_UP:
      editor_page_move(-static_cast<int>(EDITOR_VISIBLE_LINES));
      return;
    case INPUT_EVENT_PAGE_DOWN:
      editor_page_move(static_cast<int>(EDITOR_VISIBLE_LINES));
      return;
    case INPUT_EVENT_SAVE: {
      // Typing goes on while it saves. A save cannot change the explorer listing, so no
      // folder reload here.
      net_request_t request = net::make(NET_SAVE_DOCUMENT, TR("Sauvegarde...", "Saving..."), on_document_saved);
      request.prepare = prepare_save;
      net::submit(request);
      editor_close_overlay();
      editor_sync_preferred_visual_col();
      g_app.full_redraw_needed = true;
      return;
    }
    case INPUT_EVENT_CANCEL:
      editor_open_command_menu();
      return;
    case INPUT_EVENT_ESCAPE:
      editor_request_exit();
      return;
    default:
      return;
  }
}

void dispatch_input(const input_event_t& event) {
  log_input_event(event);
  // ANNULATION stops the request of this screen before meaning anything else.
  if (event.type == INPUT_EVENT_CANCEL && net::busy() && net::busy_owner() == g_app.screen) {
    clear_status();
    net::cancel();
    if (g_app.status_message[0] == '\0') {
      set_status(TR("Requete annulee", "Request cancelled"));  // unless the request said more (chat)
    }
    return;
  }
  switch (g_app.screen) {
    case SCREEN_HOME: handle_home_input(event); break;
    case SCREEN_FILES: handle_files_input(event); break;
    case SCREEN_WIFI: handle_wifi_input(event); break;
    case SCREEN_SETTINGS: handle_settings_input(event); break;
    case SCREEN_CHAT: handle_chat_input(event); break;
    case SCREEN_TEXT_INPUT: handle_text_input(event); break;
    case SCREEN_EDITOR: handle_editor_input(event); break;
    case SCREEN_MODEL_PICKER: model_picker_handle(event); break;
    case SCREEN_SYSTEM: system_screen_handle(event); break;
    case SCREEN_PROJECTS: projects_screen_handle(event); break;
    case SCREEN_PROJECT: project_screen_handle(event); break;
    case SCREEN_TASKS: tasks_screen_handle(event); break;
    case SCREEN_SEARCH: search_screen_handle(event); break;
    case SCREEN_LOCK: lock_screen_handle(event); break;
    case SCREEN_BOOT: break;
  }
}

// The status line timer of a request changes every second: redraw then, and once more when
// the request ends. Only the changed characters are sent.
void refresh_busy_status() {
  char busy[sizeof(g_app.shown_busy)];
  busy[0] = '\0';
  if (net::busy() && net::busy_owner() == g_app.screen) {
    net::busy_text(busy, sizeof(busy));
  }
  if (strcmp(busy, g_app.shown_busy) != 0) {
    g_app.full_redraw_needed = true;
  }
}

// Network updates (`pio run -e ota -t upload`) use the docs API token as password; with no
// token they stay off.
void start_lan_updates() {
  g_ota_service.begin_lan(OTA_HOSTNAME, g_settings.api_token, system_screen_show_update_progress);
}

void initialize_state() {
  memset(&g_app, 0, sizeof(g_app));
  memset(g_chat_render_lines, 0, sizeof(g_chat_render_lines));
  safe_copy(g_app.explorer.current_folder, sizeof(g_app.explorer.current_folder), "/");
  g_app.screen = SCREEN_HOME;
  g_app.full_redraw_needed = true;
  g_app.topbar_dirty = true;
  app_log_info("app", "state initialized current_folder=\"%s\"", g_app.explorer.current_folder);
}
}  // namespace

namespace shell {

void set_status(const char* message) {
  ::set_status(message);
}

void clear_status() {
  ::clear_status();
}

void show_busy_status(const char* message) {
  ::show_busy_status(message);
}

void switch_screen(screen_id_t screen) {
  ::switch_screen(screen);
}

screen_id_t current_screen() {
  return g_app.screen;
}

screen_id_t request_owner() {
  return g_app.screen == SCREEN_TEXT_INPUT ? g_app.text_input.return_screen : g_app.screen;
}

void request_redraw() {
  g_app.full_redraw_needed = true;
}

void render_frame(const char* screen_name, const char* footer) {
  render_frame_with_current_topbar(screen_name, footer);
}

void render_status_line() {
  ::render_status_line();
}

void request_text_input(screen_id_t return_screen,
                        const char* title,
                        const char* prompt,
                        const char* initial_value,
                        bool masked,
                        text_input_done_t on_done) {
  ::request_text_input(TEXT_INPUT_CALLBACK, return_screen, title, prompt, initial_value, masked);
  g_app.text_input.on_done = on_done;
}

void open_document_from(const char* id, uint16_t line, screen_id_t return_screen) {
  request_document(id, line, return_screen);
}

uint32_t last_input_ms() {
  return g_app.last_input_ms;
}

app_settings_t& settings() {
  return g_settings;
}

SettingsService& settings_service() {
  return g_settings_service;
}

ClockService& clock_service() {
  return g_clock_service;
}

WifiService& wifi() {
  return g_wifi_service;
}

OtaService& ota() {
  return g_ota_service;
}

}  // namespace shell

extern "C" void app_setup(void) {
  serial_begin(115200);
  delay_ms(50);
  app_log_set_level(APP_LOG_LEVEL_INFO);
  app_log_info("boot", "miniOS start");

  initialize_state();
  input_parser_init(&g_input_parser);
  minitel_ui_begin(MINITEL_POWER_ON_BAUD, MINITEL_RX_PIN, MINITEL_TX_PIN);
  minitel_link_setup(MINITEL_LINK_BAUD);
  log_link_state("boot");

  app_log_info("boot", "load settings");
  g_settings_service.begin();
  g_settings_service.load(&g_settings);
  workspace::begin();
  net::begin();

  // Nothing waits for Wi-Fi: the network task joins the saved network, and the link watch in
  // app_loop starts NTP, network updates and the server check once the link is up.
  app_log_info("boot", "init wifi");
  g_wifi_service.begin();
  const bool joining = connect_saved_wifi();
  if (!joining) {
    check_store(nullptr);  // offline from the start: the screens say so
  }
  app_log_info("boot", "init clock");
  g_clock_service.begin(g_settings.timezone);

  // The logo takes a while: the network task connects meanwhile.
  render_boot_screen(joining ? TR("Connexion WiFi en arriere-plan", "Joining WiFi in the background")
                             : TR("Mode hors ligne", "Offline mode"),
                     g_settings.wifi_ssid[0] ? g_settings.wifi_ssid
                                             : TR("Aucun reseau enregistre", "No saved network"));

  g_app.screen = SCREEN_HOME;
  g_app.full_redraw_needed = true;
  clear_status();
  if (lock_screen_required()) {
    lock_screen_open();
  }
  app_log_info("boot", "setup complete screen=%s", current_screen_label());
}

extern "C" void app_loop(void) {
  const bool wifi_connected = g_wifi_service.is_connected();
  if (wifi_connected != g_app.last_wifi_connected) {
    app_log_info("wifi", "link=%s", wifi_connected ? "up" : "down");
    g_app.last_wifi_connected = wifi_connected;
    if (wifi_connected) {
      start_lan_updates();
    }
    check_store(nullptr);
    if (g_app.screen == SCREEN_FILES) {
      // Unless a request of the screen is pending (a listing asked for during the boot
      // connect): it runs anyway, and a second one would be refused ("Requete en cours").
      if (!net::busy()) {
        refresh_explorer(nullptr);
      }
    } else if (g_app.screen == SCREEN_CHAT && !wifi_connected) {
      set_status(TR("WiFi hors ligne", "WiFi offline"));
    }
  }
  g_clock_service.update(wifi_connected);  // NTP once the link is up
  // A server update owns the flash while it downloads.
  if (!net::busy() || net::busy_kind() != NET_INSTALL_UPDATE) {
    g_ota_service.handle();
  }
  net::poll();
  chat_poll_reply();
  if (g_app.screen != SCREEN_LOCK) {
    workspace::tick();
  }

  char topbar_line[41];
  build_topbar_line(topbar_line, sizeof(topbar_line));
  if (strcmp(topbar_line, g_app.last_topbar) != 0) {
    g_app.full_redraw_needed = true;
  }

  handle_link_event(minitel_link_poll(MINITEL_LINK_BAUD));
  // A receive error means bytes at another speed: garbage, left to the next poll.
  while (!uart_minitel_rx_error() && uart_minitel_available()) {
    input_event_t event{};
    const uint8_t sequence_state = g_input_parser.sequence_state;
    const uint8_t byte = uart_minitel_read();
    log_input_byte(byte, sequence_state);
    if (sequence_state == 1) {
      minitel_link_note_status(byte);
    }
    if (!terminal_wake_detection_enabled()) {
      reset_terminal_wake_detector();
    } else if (detect_terminal_wake_sequence(sequence_state, byte)) {
      request_terminal_wake_redraw();
    }
    if (input_parser_feed(&g_input_parser, byte, &event)) {
      g_app.last_input_ms = get_millis();
      dispatch_input(event);
    }
  }

  refresh_busy_status();
  if (g_app.full_redraw_needed && !minitel_link_holding_output()) {
    // With the echo filter our own bytes come back while drawing, and could pass for a key.
    render_current_screen(!minitel_link_state().echo_filter);
  }

  delay_ms(10);
}
