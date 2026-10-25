#ifndef APP_NET_REQUESTS_H
#define APP_NET_REQUESTS_H

#include <stddef.h>
#include <stdint.h>

#include "lib_minitel/net/wifi.h"
#include "services/chat_service_utils.h"
#include "services/settings_service.h"
#include "storage/document_store.h"
#include "storage/workspace_utils.h"

// Network requests of the miniOS: Bibliotek, AI chat, server updates, Wi-Fi scan and
// connect. They run one at a time on the network task (services/net_worker.h) while the loop
// keeps drawing and reading keys; each one reports back through its callback, on the loop.
//
// Foreground requests are the user's: the status line shows them with a timer, ANNULATION
// cancels them, and so does leaving the screen they belong to. Background requests (the boot
// Wi-Fi connect, today's tasks, server health) stay silent, nothing cancels them, and they
// wait for foreground ones; a request already running is never interrupted by another.

constexpr uint8_t NET_MAX_FOLDERS = 16;
constexpr uint8_t NET_MAX_DOCUMENTS = 16;
constexpr uint8_t NET_MAX_PROJECTS = 24;
constexpr uint8_t NET_MAX_TASKS = 30;
constexpr uint8_t NET_MAX_HITS = 20;
constexpr uint8_t NET_MAX_MODELS = 40;
constexpr uint8_t NET_MAX_NETWORKS = 16;
constexpr size_t NET_MAX_LINE = CHAT_MAX_PROMPT_LENGTH + 24;  // typed text plus a timestamp
constexpr size_t NET_MAX_QUERY = 40;
constexpr size_t NET_MAX_ERROR = 96;
constexpr size_t NET_MAX_VERSION = 24;
constexpr size_t NET_MAX_NOW = 16;  // "YYYY-MM-DD HH:MM"

enum net_kind_t : uint8_t {
  NET_HEALTH = 0,
  NET_LIST_FOLDER,
  NET_GET_DOCUMENT,
  NET_SAVE_DOCUMENT,
  NET_CREATE_DOCUMENT,  // creates it, then reads it back for the editor
  NET_CREATE_FOLDER,
  NET_UPDATE_TAGS,
  NET_APPEND_LINE,
  NET_LIST_PROJECTS,
  NET_CREATE_PROJECT,
  NET_LIST_TASKS,
  NET_ADD_TASK,
  NET_SET_TASK_DONE,
  NET_SEARCH,
  NET_CHAT,
  NET_CHAT_CONFIRM,  // agent mode: the user's answer to the assistant's confirmation
  NET_LIST_MODELS,
  NET_CHECK_UPDATE,
  NET_INSTALL_UPDATE,
  NET_WIFI_SCAN,
  // Failed or cancelled: back to the saved network. In the background (boot, the saved
  // network): nothing to do when the link is already up.
  NET_WIFI_CONNECT,
};

struct net_request_t;
struct net_reply_t;

// Loop: the request finished, `reply.ok` tells how. `reply` is only valid during the call.
typedef void (*net_done_t)(const net_request_t& request, const net_reply_t& reply);
// Loop: the foreground request was cancelled (ANNULATION, or its screen was left).
typedef void (*net_cancelled_t)(const net_request_t& request);
// Loop, right before the request starts: copy its large inputs into `data`.
typedef void (*net_prepare_t)(net_request_t& request, net_reply_t& reply);

struct net_request_t {
  net_kind_t kind;
  bool background;
  const char* label;  // status line while it runs (string literal)
  net_done_t on_done;
  net_cancelled_t on_cancelled;
  net_prepare_t prepare;
  const char* status;  // for the callback: status line on success (string literal)
  uint32_t context;    // free for the caller, given back to the callbacks
  uint8_t owner;       // screen it belongs to, set by net::submit()
  uint32_t submitted_ms;
  union {
    struct {
      char folder[STORE_MAX_FOLDER_PATH + 1];
      char tag[STORE_MAX_TAG_LENGTH + 1];
      char select[STORE_MAX_FOLDER_PATH + 1];  // folder to select once listed
    } listing;
    struct {
      char id[STORE_MAX_DOCUMENT_ID + 1];
      uint16_t line;  // 1-based, 0 = top, past the end = end
      uint8_t return_screen;
    } open;
    struct {
      uint32_t revision;  // editor revision copied by the save, see prepare
    } save;
    struct {
      char folder[STORE_MAX_FOLDER_PATH + 1];
      char name[CHAT_MAX_PROMPT_LENGTH + 1];
    } create;
    struct {
      char id[STORE_MAX_DOCUMENT_ID + 1];
      char csv[CHAT_MAX_PROMPT_LENGTH + 1];
    } tags;
    struct {
      char id[STORE_MAX_DOCUMENT_ID + 1];
      char text[NET_MAX_LINE + 1];
    } append;
    struct {
      char date[WS_DATE_LENGTH + 1];  // today, for the overdue counts
    } projects;
    struct {
      char title[CHAT_MAX_PROMPT_LENGTH + 1];
    } project;
    struct {
      char project[WS_MAX_PROJECT_NAME + 1];  // "" for every project
      bool due_today;                         // due today or late, else every open task
      char date[WS_DATE_LENGTH + 1];
      uint8_t max;
    } tasks;
    struct {
      char project[WS_MAX_PROJECT_NAME + 1];
      task_input_t task;
    } add_task;
    struct {
      char file[STORE_MAX_DOCUMENT_ID + 1];
      uint16_t line;
      bool done;
    } toggle;
    struct {
      char query[NET_MAX_QUERY + 1];
    } search;
    struct {
      char prompt[CHAT_MAX_PROMPT_LENGTH + 1];
      bool agent;                  // to Bibliotek's assistant, with tools on the notes
      char now[NET_MAX_NOW + 1];  // agent: local time, "" before NTP
    } chat;
    struct {
      char id[CHAT_MAX_CONFIRM_ID + 1];
      bool accept;
    } confirm;
    struct {
      char ssid[SETTINGS_MAX_WIFI_SSID + 1];
      char password[SETTINGS_MAX_WIFI_PASSWORD + 1];  // "" for an open network
      // The saved network, "" for none: a failed or cancelled connect goes back to it.
      char saved_ssid[SETTINGS_MAX_WIFI_SSID + 1];
      char saved_password[SETTINGS_MAX_WIFI_PASSWORD + 1];
    } wifi;
  } params;
};

// Filled on the network task, read by the loop in the callbacks. One at a time: it belongs
// to the request in flight.
struct net_reply_t {
  bool ok;
  char error[NET_MAX_ERROR];  // short French message when !ok
  int count;                  // items listed
  int total;                  // tasks: open tasks in all
  bool truncated;             // chat: the reply is cut
  bool store_available;       // Bibliotek answered
  union {
    struct {
      folder_info_t folders[NET_MAX_FOLDERS];
      document_summary_t documents[NET_MAX_DOCUMENTS];
      int folder_count;
      int document_count;
    } listing;
    document_record_t document;
    project_summary_t projects[NET_MAX_PROJECTS];
    char project_name[WS_MAX_PROJECT_NAME + 1];
    task_item_t tasks[NET_MAX_TASKS];
    search_hit_t hits[NET_MAX_HITS];
    // NET_CHAT and NET_CHAT_CONFIRM. Prepare fills `text` and `actions` with what the reply
    // already holds (empty for a new question): the network task goes on after it.
    struct {
      chat_history_t history;  // copied by prepare (NET_CHAT)
      char text[CHAT_MAX_REPLY_LENGTH + 1];
      char actions[CHAT_MAX_ACTIONS_LENGTH + 1];  // agent: one per line
      chat_confirm_t confirm;  // agent: id set when the assistant waits for the user
    } chat;
    char models[NET_MAX_MODELS][CHAT_MAX_MODEL_ID_LENGTH + 1];
    struct {
      char version[NET_MAX_VERSION];
      bool newer;
    } update;
    lm_wifi_network_t networks[NET_MAX_NETWORKS];
  } data;
};

namespace net {

// Starts the network task. Before any submit.
bool begin();
// Every loop iteration: runs the callbacks of finished requests, starts the next one.
void poll();

// A request to complete and submit.
net_request_t make(net_kind_t kind, const char* label, net_done_t on_done);
// Submits it as a foreground request of the current screen, or in the background. Returns
// false when it is refused: another foreground request is pending (the status line says
// so), or the queue is full.
bool submit(net_request_t& request);
// A background request of `kind` is queued or running.
bool background_pending(net_kind_t kind);

// A foreground request is pending.
bool busy();
// Screen of the pending foreground request.
uint8_t busy_owner();
// Kind of the pending foreground request.
net_kind_t busy_kind();
// ANNULATION: cancels the foreground request. False when there was none.
bool cancel();
// The user moved to `screen`: a foreground request of another screen is cancelled.
void screen_changed(uint8_t screen);
// Status line while the foreground request runs: "Chargement... 3s  ANN annule".
void busy_text(char* out, size_t out_size);

// The running chat request (a question or a confirmation): copies its reply and its
// actions so far. False when none runs: the caller keeps what it has.
bool chat_progress(char* text, size_t text_size, char* actions, size_t actions_size);

// Outcome of the last Bibliotek / AI request.
bool store_available();
const char* store_error();
const char* chat_error();

uint32_t stack_free_bytes();

}  // namespace net

#endif
