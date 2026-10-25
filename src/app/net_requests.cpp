#include "net_requests.h"

#include <Arduino.h>
#include <stdio.h>
#include <string.h>

#include <atomic>

#include "app_shell.h"
#include "lang.h"
#include "platform/app_log.h"
#include "services/chat_service.h"
#include "services/net_worker.h"
#include "services/ota_service.h"
#include "services/settings_service.h"
#include "services/wifi_service.h"
#include "services/wifi_utils.h"
#include "storage/api_document_store.h"
#include "ui/minitel_ui.h"

namespace {

constexpr const char* CANCEL_HINT = TR(" ANN annule", " ANN cancels");
// Progress stage of NET_WIFI_CONNECT once it goes back to the saved network.
constexpr uint32_t WIFI_STAGE_BACK_TO_SAVED = 1;

// Settings the network task needs, copied on the loop right before each request.
struct net_config_t {
  char api_url[SETTINGS_MAX_API_URL + 1];
  char api_token[SETTINGS_MAX_API_TOKEN + 1];
  char ai_url[SETTINGS_MAX_AI_BASE_URL + 1];
  char ai_key[SETTINGS_MAX_AI_API_KEY + 1];
  char ai_model[SETTINGS_MAX_AI_MODEL + 1];
};

// Handed over with the slot (see net_worker.h): written by the loop before the request
// starts, used by the network task while it runs, read back by the loop in the callbacks.
net_request_t g_requests[net_queue::NET_QUEUE_SLOTS];
net_reply_t g_reply;
net_config_t g_config;

// Network task only: its own service instances, configured from g_config.
ApiDocumentStore g_net_store;
ChatService g_net_chat;
OtaService g_net_ota;
WifiService g_net_wifi;
net_config_t g_applied_config;

// Length of the actions of the running chat request, published after they are written (the
// reply's own length is the worker's progress amount).
std::atomic<uint32_t> g_chat_actions_length(0);

// Loop only: outcome of the last requests, for the screens.
bool g_store_available = false;
char g_store_error[NET_MAX_ERROR] = "";
char g_chat_error[NET_MAX_ERROR] = "";

void safe_copy(char* dest, size_t dest_size, const char* src) {
  if (!dest || dest_size == 0) {
    return;
  }
  strncpy(dest, src ? src : "", dest_size - 1);
  dest[dest_size - 1] = '\0';
}

const char* kind_label(net_kind_t kind) {
  switch (kind) {
    case NET_HEALTH: return "health";
    case NET_LIST_FOLDER: return "list_folder";
    case NET_GET_DOCUMENT: return "get_document";
    case NET_SAVE_DOCUMENT: return "save_document";
    case NET_CREATE_DOCUMENT: return "create_document";
    case NET_CREATE_FOLDER: return "create_folder";
    case NET_UPDATE_TAGS: return "update_tags";
    case NET_APPEND_LINE: return "append_line";
    case NET_LIST_PROJECTS: return "list_projects";
    case NET_CREATE_PROJECT: return "create_project";
    case NET_LIST_TASKS: return "list_tasks";
    case NET_ADD_TASK: return "add_task";
    case NET_SET_TASK_DONE: return "set_task_done";
    case NET_SEARCH: return "search";
    case NET_CHAT: return "chat";
    case NET_CHAT_CONFIRM: return "chat_confirm";
    case NET_LIST_MODELS: return "list_models";
    case NET_CHECK_UPDATE: return "check_update";
    case NET_INSTALL_UPDATE: return "install_update";
    case NET_WIFI_SCAN: return "wifi_scan";
    case NET_WIFI_CONNECT: return "wifi_connect";
  }
  return "?";
}

bool is_store_request(net_kind_t kind) {
  switch (kind) {
    case NET_CHAT:
    case NET_CHAT_CONFIRM:
    case NET_LIST_MODELS:
    case NET_CHECK_UPDATE:
    case NET_INSTALL_UPDATE:
    case NET_WIFI_SCAN:
    case NET_WIFI_CONNECT:
      return false;
    default:
      return true;
  }
}

// A reply that streams into reply.data.chat.
bool is_chat_stream(net_kind_t kind) {
  return kind == NET_CHAT || kind == NET_CHAT_CONFIRM;
}

bool is_chat_request(net_kind_t kind) {
  return is_chat_stream(kind) || kind == NET_LIST_MODELS;
}

// ---- Network task ---------------------------------------------------------------------

bool should_stop() {
  return net_worker::cancel_requested();
}

void on_chat_progress(chat_stage_t stage, size_t length, size_t actions_length) {
  g_chat_actions_length.store(static_cast<uint32_t>(actions_length));
  net_worker::report(static_cast<uint32_t>(stage), static_cast<uint32_t>(length), nullptr);
}

void on_update_progress(const char* message, int percent) {
  net_worker::report(0, percent > 0 ? static_cast<uint32_t>(percent) : 0, message);
}

void apply_config() {
  if (strcmp(g_config.api_url, g_applied_config.api_url) != 0 ||
      strcmp(g_config.api_token, g_applied_config.api_token) != 0) {
    g_net_store.configure(g_config.api_url, g_config.api_token);
    g_net_chat.configure_assistant(g_config.api_url, g_config.api_token);
    safe_copy(g_applied_config.api_url, sizeof(g_applied_config.api_url), g_config.api_url);
    safe_copy(g_applied_config.api_token, sizeof(g_applied_config.api_token), g_config.api_token);
  }
  if (strcmp(g_config.ai_url, g_applied_config.ai_url) != 0 ||
      strcmp(g_config.ai_key, g_applied_config.ai_key) != 0 ||
      strcmp(g_config.ai_model, g_applied_config.ai_model) != 0) {
    g_net_chat.configure(g_config.ai_url, g_config.ai_key, g_config.ai_model);
    safe_copy(g_applied_config.ai_url, sizeof(g_applied_config.ai_url), g_config.ai_url);
    safe_copy(g_applied_config.ai_key, sizeof(g_applied_config.ai_key), g_config.ai_key);
    safe_copy(g_applied_config.ai_model, sizeof(g_applied_config.ai_model), g_config.ai_model);
  }
}

void run_store_request(const net_request_t& request, net_reply_t& reply) {
  switch (request.kind) {
    case NET_HEALTH:
      g_net_store.begin();
      reply.ok = g_net_store.is_available();
      break;

    case NET_LIST_FOLDER: {
      auto& listing = reply.data.listing;
      listing.document_count = 0;
      listing.folder_count =
          g_net_store.list_folders(request.params.listing.folder, listing.folders, NET_MAX_FOLDERS);
      if (g_net_store.is_available()) {
        listing.document_count = g_net_store.list_documents(
            request.params.listing.folder, request.params.listing.tag, listing.documents, NET_MAX_DOCUMENTS);
      }
      reply.ok = g_net_store.is_available();
      break;
    }

    case NET_GET_DOCUMENT:
      reply.ok = g_net_store.get_document(request.params.open.id, &reply.data.document);
      break;

    case NET_SAVE_DOCUMENT:
      reply.ok = g_net_store.save_document(reply.data.document);
      break;

    case NET_CREATE_DOCUMENT:
      reply.ok = g_net_store.create_document(
          request.params.create.folder, request.params.create.name, &reply.data.document);
      if (reply.ok) {
        // Read it back as the editor would open it (get_document rewrites its output).
        char id[STORE_MAX_DOCUMENT_ID + 1];
        safe_copy(id, sizeof(id), reply.data.document.id);
        reply.ok = g_net_store.get_document(id, &reply.data.document);
      }
      break;

    case NET_CREATE_FOLDER:
      reply.ok = g_net_store.create_folder(request.params.create.folder, request.params.create.name);
      break;

    case NET_UPDATE_TAGS:
      reply.ok = g_net_store.update_tags(request.params.tags.id, request.params.tags.csv);
      break;

    case NET_APPEND_LINE:
      reply.ok = g_net_store.append_line(request.params.append.id, request.params.append.text);
      break;

    case NET_LIST_PROJECTS: {
      const int count = g_net_store.list_projects(request.params.projects.date, reply.data.projects, NET_MAX_PROJECTS);
      reply.ok = count >= 0;
      reply.count = count > 0 ? count : 0;
      break;
    }

    case NET_CREATE_PROJECT:
      reply.ok = g_net_store.create_project(
          request.params.project.title, reply.data.project_name, sizeof(reply.data.project_name));
      break;

    case NET_LIST_TASKS: {
      const uint8_t max = request.params.tasks.max > 0 && request.params.tasks.max <= NET_MAX_TASKS
                              ? request.params.tasks.max
                              : NET_MAX_TASKS;
      const int count = g_net_store.list_tasks(request.params.tasks.project[0] ? request.params.tasks.project : nullptr,
                                               request.params.tasks.due_today ? "today" : nullptr,
                                               request.params.tasks.date,
                                               reply.data.tasks,
                                               max,
                                               &reply.total);
      reply.ok = count >= 0;
      reply.count = count > 0 ? count : 0;
      break;
    }

    case NET_ADD_TASK:
      reply.ok = g_net_store.add_task(request.params.add_task.project, request.params.add_task.task);
      break;

    case NET_SET_TASK_DONE:
      reply.ok = g_net_store.set_task_done(
          request.params.toggle.file, request.params.toggle.line, request.params.toggle.done);
      break;

    case NET_SEARCH: {
      const int count = g_net_store.search(request.params.search.query, reply.data.hits, NET_MAX_HITS);
      reply.ok = count >= 0;
      reply.count = count > 0 ? count : 0;
      break;
    }

    default:
      break;
  }

  reply.store_available = g_net_store.is_available();
  if (!reply.ok) {
    safe_copy(reply.error, sizeof(reply.error),
              g_net_store.last_error()[0] != '\0' ? g_net_store.last_error()
                                                  : TR("API indisponible", "API unavailable"));
  }
}

void run_wifi_request(const net_request_t& request, net_reply_t& reply) {
  if (should_stop()) {
    // The radio stays as it is.
    safe_copy(reply.error, sizeof(reply.error), TR("Requete annulee", "Request cancelled"));
    return;
  }
  if (request.kind == NET_WIFI_SCAN) {
    reply.count = g_net_wifi.scan(reply.data.networks, NET_MAX_NETWORKS);
    reply.ok = true;
    return;
  }

  const auto& wifi = request.params.wifi;
  if (request.background && g_net_wifi.is_connected()) {
    reply.ok = true;  // boot connect: a connect the user asked for went first, keep its network
    return;
  }
  // A cancel that comes as the link comes up still means "not this network".
  reply.ok = g_net_wifi.connect(wifi.ssid, wifi.password, should_stop) && !should_stop();
  if (reply.ok) {
    return;
  }
  safe_copy(reply.error, sizeof(reply.error), TR("Connexion WiFi KO", "WiFi connection failed"));
  switch (wifi_utils::after_failed_connect(wifi.ssid, wifi.saved_ssid, should_stop())) {
    case wifi_utils::AFTER_CONNECT_BACK_TO_SAVED:
      // Once cancelled, this only starts the connection: the radio completes it alone.
      net_worker::report(WIFI_STAGE_BACK_TO_SAVED, 0, nullptr);
      g_net_wifi.connect(wifi.saved_ssid, wifi.saved_password, should_stop);
      break;
    case wifi_utils::AFTER_CONNECT_STOP:
      g_net_wifi.disconnect();
      break;
    case wifi_utils::AFTER_CONNECT_STAY:
      break;
  }
}

void run_request(int8_t slot) {
  const net_request_t& request = g_requests[slot];
  net_reply_t& reply = g_reply;
  reply.ok = false;
  reply.error[0] = '\0';
  reply.count = 0;
  reply.total = 0;
  reply.truncated = false;
  reply.store_available = false;
  const uint32_t started_ms = millis();
  apply_config();

  switch (request.kind) {
    case NET_CHAT:
    case NET_CHAT_CONFIRM: {
      chat_stream_hooks_t hooks;
      hooks.cancelled = should_stop;
      hooks.progress = on_chat_progress;
      auto& chat = reply.data.chat;
      chat_agent_reply_t agent_reply;
      agent_reply.text = chat.text;
      agent_reply.text_size = sizeof(chat.text);
      agent_reply.actions = chat.actions;
      agent_reply.actions_size = sizeof(chat.actions);
      agent_reply.confirm = &chat.confirm;
      if (request.kind == NET_CHAT_CONFIRM) {
        reply.ok = g_net_chat.answer_confirmation(
            request.params.confirm.id, request.params.confirm.accept, agent_reply, hooks, &reply.truncated);
      } else if (request.params.chat.agent) {
        reply.ok = g_net_chat.stream_agent(
            chat.history, request.params.chat.prompt, request.params.chat.now, agent_reply, hooks, &reply.truncated);
      } else {
        reply.ok = g_net_chat.stream_message(
            chat.history, request.params.chat.prompt, chat.text, sizeof(chat.text), hooks, &reply.truncated);
      }
      if (!reply.ok) {
        safe_copy(reply.error, sizeof(reply.error), g_net_chat.last_error());
      }
      break;
    }

    case NET_LIST_MODELS: {
      const int count = g_net_chat.list_models(reply.data.models, NET_MAX_MODELS);
      reply.ok = count >= 0;
      reply.count = count > 0 ? count : 0;
      if (!reply.ok) {
        safe_copy(reply.error, sizeof(reply.error), g_net_chat.last_error());
      }
      break;
    }

    case NET_CHECK_UPDATE:
      reply.data.update.newer = false;
      reply.ok = g_net_ota.check_server(g_config.api_url,
                                        g_config.api_token,
                                        reply.data.update.version,
                                        sizeof(reply.data.update.version),
                                        &reply.data.update.newer);
      if (!reply.ok) {
        safe_copy(reply.error, sizeof(reply.error), g_net_ota.last_error());
      }
      break;

    case NET_INSTALL_UPDATE:
      // Reboots when it succeeds.
      g_net_ota.install_from_server(g_config.api_url, g_config.api_token, on_update_progress, should_stop);
      safe_copy(reply.error, sizeof(reply.error), g_net_ota.last_error());
      break;

    case NET_WIFI_SCAN:
    case NET_WIFI_CONNECT:
      run_wifi_request(request, reply);
      break;

    default:
      run_store_request(request, reply);
      break;
  }

  // Headroom check: the TLS session is gone now, the lowest marks stay.
  app_log_info("net",
               "%s %s in %lu ms, stack free %lu, heap %lu (min %lu)",
               kind_label(request.kind),
               reply.ok ? "ok" : reply.error,
               static_cast<unsigned long>(millis() - started_ms),
               static_cast<unsigned long>(net_worker::stack_free_bytes()),
               static_cast<unsigned long>(ESP.getFreeHeap()),
               static_cast<unsigned long>(ESP.getMinFreeHeap()));
}

// ---- Loop -----------------------------------------------------------------------------

// A Wi-Fi connect holds the network task (the boot one, or a cancelled one going back to the
// saved network): up to 15 s when the network is away.
bool wifi_connect_in_flight() {
  const int8_t slot = net_worker::in_flight();
  return slot != net_queue::NET_QUEUE_NONE && g_requests[slot].kind == NET_WIFI_CONNECT;
}

void prepare_request(int8_t slot) {
  const app_settings_t& settings = shell::settings();
  safe_copy(g_config.api_url, sizeof(g_config.api_url), settings.api_url);
  safe_copy(g_config.api_token, sizeof(g_config.api_token), settings.api_token);
  safe_copy(g_config.ai_url, sizeof(g_config.ai_url), settings.ai_base_url);
  safe_copy(g_config.ai_key, sizeof(g_config.ai_key), settings.ai_api_key);
  safe_copy(g_config.ai_model, sizeof(g_config.ai_model), settings.ai_model);

  net_request_t& request = g_requests[slot];
  if (request.prepare) {
    request.prepare(request, g_reply);
  }
  if (is_chat_stream(request.kind)) {
    // What the reply already holds (after a confirmation) shows while the request starts. The
    // network task is not running a request yet: the loop may publish it.
    g_chat_actions_length.store(static_cast<uint32_t>(strlen(g_reply.data.chat.actions)));
    net_worker::report(CHAT_STAGE_CONNECTING, static_cast<uint32_t>(strlen(g_reply.data.chat.text)), nullptr);
  }
}

void finish_request(int8_t slot) {
  const net_request_t& request = g_requests[slot];
  const net_reply_t& reply = g_reply;
  if (is_store_request(request.kind)) {
    g_store_available = reply.store_available;
    safe_copy(g_store_error, sizeof(g_store_error), reply.ok ? "" : reply.error);
  } else if (is_chat_request(request.kind)) {
    safe_copy(g_chat_error, sizeof(g_chat_error), reply.ok ? "" : reply.error);
  }
  if (!request.background) {
    shell::request_redraw();  // the status line drops the timer
  }
  if (request.on_done) {
    request.on_done(request, reply);
  }
}

}  // namespace

namespace net {

bool begin() {
  return net_worker::begin(prepare_request, run_request, finish_request);
}

void poll() {
  net_worker::poll();
}

net_request_t make(net_kind_t kind, const char* label, net_done_t on_done) {
  net_request_t request;
  memset(&request, 0, sizeof(request));
  request.kind = kind;
  request.label = label;
  request.on_done = on_done;
  return request;
}

bool submit(net_request_t& request) {
  request.owner = static_cast<uint8_t>(shell::request_owner());
  request.submitted_ms = millis();
  const int8_t slot = net_worker::submit(
      request.kind, request.background ? net_queue::PRIORITY_BACKGROUND : net_queue::PRIORITY_FOREGROUND);
  if (slot == net_queue::NET_QUEUE_NONE) {
    if (!request.background) {
      shell::set_status(net_worker::started() ? TR("Requete en cours: ANN annule", "Request running: ANN cancels")
                                              : TR("Reseau indisponible", "Network unavailable"));
    }
    app_log_warn("net", "%s refused", kind_label(request.kind));
    return false;
  }
  g_requests[slot] = request;  // a merged background request takes the newer parameters
  if (!request.background) {
    shell::request_redraw();  // shows the timer on the status line
  }
  net_worker::pump();
  return true;
}

bool background_pending(net_kind_t kind) {
  return net_worker::background_pending(kind);
}

bool busy() {
  return net_worker::foreground() != net_queue::NET_QUEUE_NONE;
}

uint8_t busy_owner() {
  const int8_t slot = net_worker::foreground();
  return slot != net_queue::NET_QUEUE_NONE ? g_requests[slot].owner : 0;
}

net_kind_t busy_kind() {
  const int8_t slot = net_worker::foreground();
  return slot != net_queue::NET_QUEUE_NONE ? g_requests[slot].kind : NET_HEALTH;
}

bool cancel() {
  const int8_t slot = net_worker::foreground();
  if (slot == net_queue::NET_QUEUE_NONE) {
    return false;
  }
  // The callback may submit again and reuse the slot: work on a copy.
  const net_request_t request = g_requests[slot];
  const net_queue::cancel_result_t result = net_worker::cancel_foreground();
  app_log_info("net",
               "cancel %s (%s)",
               kind_label(request.kind),
               result == net_queue::CANCEL_STOPPING ? "running" : "queued");
  if (request.on_cancelled) {
    request.on_cancelled(request);
  }
  shell::request_redraw();
  return true;
}

void screen_changed(uint8_t screen) {
  const int8_t slot = net_worker::foreground();
  if (slot != net_queue::NET_QUEUE_NONE && g_requests[slot].owner != screen) {
    cancel();
  }
}

void busy_text(char* out, size_t out_size) {
  if (!out || out_size == 0) {
    return;
  }
  out[0] = '\0';
  const int8_t slot = net_worker::foreground();
  if (slot == net_queue::NET_QUEUE_NONE) {
    return;
  }
  const net_request_t& request = g_requests[slot];
  const bool running = net_worker::in_flight() == slot;
  net_worker::progress_t progress = {};
  if (running) {
    progress = net_worker::progress();
  }

  const char* label = request.label ? request.label : TR("Requete...", "Request...");
  bool show_seconds = true;
  if (is_chat_stream(request.kind) && running) {
    if (progress.stage == CHAT_STAGE_THINKING) {
      label = TR("L'IA reflechit...", "The AI is thinking...");
    } else if (progress.stage == CHAT_STAGE_ACTING) {
      label = TR("L'IA travaille...", "The AI is working...");
    } else if (progress.stage == CHAT_STAGE_WRITING) {
      label = TR("Reponse en cours...", "Writing the reply...");
      show_seconds = false;  // the text itself shows progress
    }
  } else if (request.kind == NET_WIFI_CONNECT && running && progress.stage == WIFI_STAGE_BACK_TO_SAVED) {
    label = TR("Retour au reseau enregistre...", "Back to the saved network...");
  } else if (!running && wifi_connect_in_flight()) {
    label = TR("Attente du WiFi...", "Waiting for WiFi...");
  }

  char head[UI_COLS + 1];
  if (progress.message) {
    snprintf(head, sizeof(head), "%s %lu%%", progress.message, static_cast<unsigned long>(progress.amount));
  } else if (show_seconds) {
    snprintf(head, sizeof(head), "%s %lus", label,
             static_cast<unsigned long>((millis() - request.submitted_ms) / 1000UL));
  } else {
    snprintf(head, sizeof(head), "%s", label);
  }
  if (strlen(head) + strlen(CANCEL_HINT) <= UI_COLS) {
    snprintf(out, out_size, "%s%s", head, CANCEL_HINT);
  } else {
    safe_copy(out, out_size, head);
  }
}

namespace {

void copy_published(char* out, size_t out_size, const char* source, size_t source_size, size_t length) {
  if (!out || out_size == 0) {
    return;
  }
  if (length >= out_size) {
    length = out_size - 1;
  }
  if (length >= source_size) {
    length = source_size - 1;
  }
  memcpy(out, source, length);
  out[length] = '\0';
}

}  // namespace

bool chat_progress(char* text, size_t text_size, char* actions, size_t actions_size) {
  // Not a cancelled one still stopping: its text belongs to an exchange already closed.
  const int8_t slot = net_worker::in_flight();
  if (slot == net_queue::NET_QUEUE_NONE || slot != net_worker::foreground() ||
      !is_chat_stream(g_requests[slot].kind)) {
    return false;
  }
  // The network task only appends to the text and the actions, and publishes their lengths
  // afterwards.
  const size_t actions_length = g_chat_actions_length.load();
  const size_t text_length = net_worker::progress().amount;
  copy_published(actions, actions_size, g_reply.data.chat.actions, sizeof(g_reply.data.chat.actions), actions_length);
  copy_published(text, text_size, g_reply.data.chat.text, sizeof(g_reply.data.chat.text), text_length);
  return true;
}

bool store_available() {
  return g_store_available;
}

const char* store_error() {
  return g_store_error;
}

const char* chat_error() {
  return g_chat_error;
}

uint32_t stack_free_bytes() {
  return net_worker::stack_free_bytes();
}

}  // namespace net
