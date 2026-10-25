#include "api_document_store.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <stdio.h>
#include <string.h>

#include "api_document_store_utils.h"
#include "app/lang.h"
#include "platform/app_log.h"
#include "services/tls_roots.h"

namespace {
constexpr uint16_t API_HTTP_TIMEOUT_MS = 4000;
constexpr size_t API_JSON_DOC_SIZE = 16384;
constexpr size_t API_CREATE_DOCUMENT_PAYLOAD_SIZE = 512;

String percent_encode(const char* input) {
  if (!input) {
    return String();
  }

  String out;
  for (size_t i = 0; input[i] != '\0'; ++i) {
    const unsigned char c = static_cast<unsigned char>(input[i]);
    const bool safe =
        (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
        c == '-' || c == '_' || c == '.' || c == '~';
    if (safe) {
      out += static_cast<char>(c);
      continue;
    }

    char encoded[4];
    snprintf(encoded, sizeof(encoded), "%%%02X", static_cast<unsigned>(c));
    out += encoded;
  }
  return out;
}

// "id=projets%2Frobot%2Ftaches.md" for the /file routes.
String file_id_query(const char* remote_folder, const char* file_name) {
  String id(remote_folder ? remote_folder : "");
  if (id.length() > 0) {
    id += '/';
  }
  id += file_name ? file_name : "";
  return String("id=") + percent_encode(id.c_str());
}
}  // namespace

ApiDocumentStore::ApiDocumentStore() : available_(false) {
  base_url_[0] = '\0';
  api_token_[0] = '\0';
  last_error_[0] = '\0';
}

void ApiDocumentStore::configure(const char* base_url, const char* api_token) {
  api_document_store_utils::safe_copy(base_url_, sizeof(base_url_), base_url);
  api_document_store_utils::safe_copy(api_token_, sizeof(api_token_), api_token);
  api_document_store_utils::trim_in_place(base_url_);
  api_document_store_utils::trim_in_place(api_token_);

  while (base_url_[0] != '\0' && base_url_[strlen(base_url_) - 1] == '/') {
    base_url_[strlen(base_url_) - 1] = '\0';
  }

  available_ = false;
  last_error_[0] = '\0';
  app_log_info("store", "api configure url=\"%s\" token=%s", base_url_[0] ? base_url_ : "<empty>", api_token_[0] ? "set" : "empty");
}

void ApiDocumentStore::clear_error() {
  available_ = true;
  last_error_[0] = '\0';
}

void ApiDocumentStore::set_error(const char* message) {
  available_ = false;
  api_document_store_utils::safe_copy(last_error_, sizeof(last_error_), message);
}

void ApiDocumentStore::set_http_error(int status_code, const char* response_body) {
  char message[sizeof(last_error_)];
  message[0] = '\0';

  switch (status_code) {
    case 400:
      snprintf(message, sizeof(message), TR("API 400 requete invalide", "API 400 invalid request"));
      break;
    case 401:
    case 403:
      snprintf(message, sizeof(message), TR("API %d token invalide", "API %d invalid token"), status_code);
      break;
    case 404:
      snprintf(message, sizeof(message), TR("API 404 ressource absente", "API 404 not found"));
      break;
    case 409:
      // A task line moved: the file changed since the list was loaded.
      snprintf(message, sizeof(message), TR("Fichier modifie: recharge", "File changed: reload"));
      break;
    case 301:
    case 302:
    case 307:
    case 308:
      // Coolify/Traefik redirect http to https: the URL must be configured as https.
      snprintf(message, sizeof(message), TR("API redirigee: utilise https://", "API redirected: use https://"));
      break;
    default:
      if (status_code >= 500) {
        snprintf(message, sizeof(message), TR("API %d erreur serveur", "API %d server error"), status_code);
      } else if (status_code > 0) {
        snprintf(message, sizeof(message), "API HTTP %d", status_code);
      } else {
        String error = HTTPClient::errorToString(status_code);
        snprintf(message,
                 sizeof(message),
                 "API %s",
                 error.length() > 0 ? error.c_str() : TR("injoignable", "unreachable"));
      }
      break;
  }

  if (response_body && response_body[0] != '\0') {
    app_log_warn("store", "api error status=%d body=%s", status_code, response_body);
  }
  set_error(message);
}

bool ApiDocumentStore::perform_request(const char* method,
                                       const char* path,
                                       const char* query,
                                       const char* json_body,
                                       bool authorize,
                                       DynamicJsonDocument* response) {
  if (base_url_[0] == '\0') {
    set_error(TR("API URL vide", "Empty API URL"));
    return false;
  }
  if (authorize && api_token_[0] == '\0') {
    set_error(TR("API token vide", "Empty API token"));
    return false;
  }
  if (WiFi.status() != WL_CONNECTED) {
    set_error(TR("WiFi hors ligne", "WiFi offline"));
    return false;
  }

  String url(base_url_);
  url += path ? path : "";
  if (query && query[0] != '\0') {
    url += '?';
    url += query;
  }

  app_log_info("store", "api %s %s", method ? method : "GET", url.c_str());

  // A server behind Coolify is reached over https: verify it against the bundled roots.
  WiFiClient plain_client;
  WiFiClientSecure secure_client;
  const bool use_tls = url_uses_tls(base_url_);
  if (use_tls) {
    secure_client.setCACert(TLS_ROOT_CERTS);
    secure_client.setHandshakeTimeout(TLS_HANDSHAKE_TIMEOUT_S);
  }
  WiFiClient& client = use_tls ? static_cast<WiFiClient&>(secure_client) : plain_client;

  HTTPClient http;
  if (!http.begin(client, url)) {
    set_error(TR("API inaccessible", "API unreachable"));
    return false;
  }

  http.setTimeout(API_HTTP_TIMEOUT_MS);
  http.addHeader("Accept", "application/json");
  if (authorize) {
    http.addHeader("Authorization", String("Bearer ") + api_token_);
  }
  if (json_body) {
    http.addHeader("Content-Type", "application/json");
  }

  int status_code = 0;
  if (strcmp(method, "GET") == 0) {
    status_code = http.GET();
  } else if (strcmp(method, "POST") == 0) {
    status_code = http.POST(json_body ? json_body : "");
  } else if (strcmp(method, "PUT") == 0) {
    status_code = http.PUT(json_body ? json_body : "");
  } else {
    http.end();
    set_error(TR("Methode HTTP invalide", "Invalid HTTP method"));
    return false;
  }

  const String raw_response = status_code > 0 ? http.getString() : String();
  http.end();

  if (status_code < 200 || status_code >= 300) {
    set_http_error(status_code, raw_response.c_str());
    return false;
  }

  if (response) {
    // Parse the response in place: copying it first would need a second heap block as
    // large as the body.
    const DeserializationError json_error = raw_response.length() > 0
                                                ? deserializeJson(*response, raw_response)
                                                : deserializeJson(*response, "null");
    if (json_error) {
      app_log_warn("store", "json error=%s body=%s", json_error.c_str(), raw_response.c_str());
      set_error(TR("JSON invalide", "Invalid JSON"));
      return false;
    }
  }

  clear_error();
  return true;
}

void ApiDocumentStore::begin() {
  if (base_url_[0] == '\0') {
    set_error(TR("API URL vide", "Empty API URL"));
    return;
  }
  if (api_token_[0] == '\0') {
    set_error(TR("API token vide", "Empty API token"));
    return;
  }

  DynamicJsonDocument response(API_JSON_DOC_SIZE);
  if (!perform_request("GET", "/health", nullptr, nullptr, false, &response)) {
    return;
  }

  JsonVariantConst root = response.as<JsonVariantConst>();
  const char* status = root["status"].is<const char*>() ? root["status"].as<const char*>() : nullptr;
  if (status && strcmp(status, "ok") == 0) {
    clear_error();
    app_log_info("store", "api health=ok");
    return;
  }

  set_error(TR("API health invalide", "Invalid API health"));
}

int ApiDocumentStore::list_folders(const char* parent_path, folder_info_t* out, int max_items) {
  if (!out || max_items <= 0) {
    return 0;
  }

  DynamicJsonDocument response(API_JSON_DOC_SIZE);
  if (!perform_request("GET", "/folders", nullptr, nullptr, true, &response)) {
    return 0;
  }

  const int count = api_document_store_utils::parse_folder_items(
      response.as<JsonVariantConst>(), parent_path, out, max_items);
  app_log_info("store", "list_folders parent=\"%s\" count=%d", parent_path ? parent_path : "/", count);
  return count;
}

int ApiDocumentStore::list_documents(const char* folder_path,
                                     const char* tag_filter,
                                     document_summary_t* out,
                                     int max_items) {
  if (!out || max_items <= 0) {
    return 0;
  }

  char remote_folder[STORE_MAX_FOLDER_PATH + 1];
  api_document_store_utils::local_folder_to_remote(folder_path, remote_folder, sizeof(remote_folder));

  // "folder=" + the path percent-encoded (up to 3 bytes per character) + NUL.
  char query[8 + 3 * STORE_MAX_FOLDER_PATH];
  query[0] = '\0';
  if (remote_folder[0] != '\0') {
    snprintf(query, sizeof(query), "folder=%s", percent_encode(remote_folder).c_str());
  }

  DynamicJsonDocument response(API_JSON_DOC_SIZE);
  if (!perform_request("GET", "/files", query, nullptr, true, &response)) {
    return 0;
  }

  const int count = api_document_store_utils::parse_document_summaries(
      response.as<JsonVariantConst>(), folder_path, tag_filter, out, max_items);
  app_log_info("store",
               "list_documents folder=\"%s\" tag=\"%s\" count=%d",
               folder_path ? folder_path : "/",
               (tag_filter && tag_filter[0]) ? tag_filter : "<none>",
               count);
  return count;
}

int ApiDocumentStore::list_tags(char out[][STORE_MAX_TAG_LENGTH + 1], int max_items) {
  if (!out || max_items <= 0) {
    return 0;
  }

  DynamicJsonDocument response(API_JSON_DOC_SIZE);
  if (!perform_request("GET", "/files", nullptr, nullptr, true, &response)) {
    return 0;
  }

  const int count = api_document_store_utils::collect_unique_tags(response.as<JsonVariantConst>(), out, max_items);
  app_log_info("store", "list_tags count=%d", count);
  return count;
}

bool ApiDocumentStore::get_document(const char* id, document_record_t* out) {
  if (!out) {
    return false;
  }

  char folder_path[STORE_MAX_FOLDER_PATH + 1];
  char file_name[STORE_MAX_FILE_NAME_LENGTH + 1];
  if (!api_document_store_utils::parse_document_id(id, folder_path, sizeof(folder_path), file_name, sizeof(file_name))) {
    set_error(TR("ID document invalide", "Invalid document id"));
    return false;
  }

  char remote_folder[STORE_MAX_FOLDER_PATH + 1];
  api_document_store_utils::local_folder_to_remote(folder_path, remote_folder, sizeof(remote_folder));

  const String query = file_id_query(remote_folder, file_name);
  DynamicJsonDocument response(API_JSON_DOC_SIZE);
  if (!perform_request("GET", "/file", query.c_str(), nullptr, true, &response)) {
    return false;
  }

  if (!api_document_store_utils::parse_document_record(response.as<JsonVariantConst>(), out, true)) {
    set_error(TR("JSON document invalide", "Invalid document JSON"));
    return false;
  }
  // The request already names the file: keep that identity so a later save writes back to
  // it, whatever naming fields the response carries.
  api_document_store_utils::safe_copy(out->id, sizeof(out->id), id);
  api_document_store_utils::safe_copy(out->folder_path, sizeof(out->folder_path), folder_path);

  clear_error();
  app_log_info("store",
               "get_document id=\"%s\" title=\"%s\"%s",
               out->id,
               out->title,
               out->truncated ? " truncated=read-only" : "");
  return true;
}

bool ApiDocumentStore::save_document(const document_record_t& document) {
  if (document.truncated) {
    // Only part of the server copy fits in RAM: writing it back would cut the document.
    // Report it without flagging the API as unavailable.
    api_document_store_utils::safe_copy(last_error_, sizeof(last_error_),
                                        TR("Doc trop long: lecture seule", "Document too long: read-only"));
    return false;
  }

  char folder_path[STORE_MAX_FOLDER_PATH + 1];
  char file_name[STORE_MAX_FILE_NAME_LENGTH + 1];
  if (!api_document_store_utils::parse_document_id(document.id,
                                                   folder_path,
                                                   sizeof(folder_path),
                                                   file_name,
                                                   sizeof(file_name))) {
    set_error(TR("ID document invalide", "Invalid document id"));
    return false;
  }

  char remote_folder[STORE_MAX_FOLDER_PATH + 1];
  api_document_store_utils::local_folder_to_remote(folder_path, remote_folder, sizeof(remote_folder));

  DynamicJsonDocument request(4096);
  request["content"] = document.body;
  request["title"] = document.title;
  request["sortOrder"] = document.version;
  JsonArray tags = request.createNestedArray("tags");
  for (uint8_t i = 0; i < document.tag_count; ++i) {
    tags.add(document.tags[i]);
  }

  String payload;
  serializeJson(request, payload);

  const String query = file_id_query(remote_folder, file_name);
  if (!perform_request("PUT", "/file", query.c_str(), payload.c_str(), true, nullptr)) {
    return false;
  }

  clear_error();
  app_log_info("store", "save_document id=\"%s\"", document.id);
  return true;
}

bool ApiDocumentStore::create_document(const char* folder_path,
                                       const char* name_or_filename,
                                       document_record_t* out) {
  char local_folder[STORE_MAX_FOLDER_PATH + 1];
  char remote_folder[STORE_MAX_FOLDER_PATH + 1];
  char file_name[STORE_MAX_FILE_NAME_LENGTH + 1];
  char title[STORE_MAX_TITLE_LENGTH + 1];
  char extension[STORE_MAX_EXTENSION_LENGTH + 1];

  api_document_store_utils::normalize_local_folder_path(folder_path, local_folder, sizeof(local_folder));
  api_document_store_utils::local_folder_to_remote(local_folder, remote_folder, sizeof(remote_folder));
  if (remote_folder[0] == '\0') {
    set_error(TR("Racine virtuelle: cree un dossier", "Virtual root: create a folder"));
    return false;
  }
  api_document_store_utils::split_name_or_filename(
      name_or_filename, file_name, sizeof(file_name), title, sizeof(title), extension, sizeof(extension));

  DynamicJsonDocument request(4096);
  request["folder"] = remote_folder;
  request["name"] = file_name;
  request["title"] = title;
  request["sortOrder"] = 0;
  JsonArray tags = request.createNestedArray("tags");
  (void)tags;

  char default_body[STORE_MAX_TITLE_LENGTH + 5];
  if (strcmp(extension, "md") == 0) {
    snprintf(default_body, sizeof(default_body), "# %s\n\n", title);
  } else {
    default_body[0] = '\0';
  }
  request["content"] = default_body;

  char payload[API_CREATE_DOCUMENT_PAYLOAD_SIZE];
  const size_t payload_size = serializeJson(request, payload, sizeof(payload));
  if (payload_size == 0 || payload_size >= sizeof(payload)) {
    set_error(TR("Payload creation trop grand", "Create payload too large"));
    return false;
  }

  DynamicJsonDocument response(API_JSON_DOC_SIZE);
  if (!perform_request("POST", "/files", nullptr, payload, true, &response)) {
    return false;
  }

  if (out) {
    if (api_document_store_utils::parse_document_record(response.as<JsonVariantConst>(), out, true)) {
      // Parsed from API response.
    } else {
      memset(out, 0, sizeof(*out));
      out->in_use = true;
      api_document_store_utils::safe_copy(out->title, sizeof(out->title), title);
      api_document_store_utils::safe_copy(out->extension, sizeof(out->extension), extension);
      api_document_store_utils::safe_copy(out->folder_path, sizeof(out->folder_path), local_folder);
      api_document_store_utils::safe_copy(out->body, sizeof(out->body), default_body);
      out->version = 0;
      api_document_store_utils::build_document_id(local_folder, file_name, out->id, sizeof(out->id));
    }
  }

  clear_error();
  app_log_info("store", "create_document folder=\"%s\" name=\"%s\"", local_folder, file_name);
  return true;
}

bool ApiDocumentStore::create_folder(const char* parent_path, const char* name) {
  char local_path[STORE_MAX_FOLDER_PATH + 1];
  char remote_path[STORE_MAX_FOLDER_PATH + 1];

  api_document_store_utils::join_local_path(parent_path, name, local_path, sizeof(local_path));
  api_document_store_utils::local_folder_to_remote(local_path, remote_path, sizeof(remote_path));
  if (remote_path[0] == '\0') {
    set_error(TR("Nom dossier invalide", "Invalid folder name"));
    return false;
  }

  DynamicJsonDocument request(512);
  request["name"] = remote_path;

  String payload;
  serializeJson(request, payload);

  if (!perform_request("POST", "/folders", nullptr, payload.c_str(), true, nullptr)) {
    return false;
  }

  clear_error();
  app_log_info("store", "create_folder path=\"%s\"", local_path);
  return true;
}

bool ApiDocumentStore::update_tags(const char* id, const char* csv_tags) {
  document_record_t document{};
  if (!get_document(id, &document)) {
    return false;
  }

  api_document_store_utils::set_tags_from_csv(&document, csv_tags);
  if (!save_document(document)) {
    return false;
  }

  clear_error();
  app_log_info("store", "update_tags id=\"%s\" tags=\"%s\"", id ? id : "<null>", csv_tags ? csv_tags : "");
  return true;
}

int ApiDocumentStore::list_projects(const char* today, project_summary_t* out, int max_items) {
  if (!out || max_items <= 0) {
    return 0;
  }
  String query;
  if (workspace_utils::is_valid_date(today)) {
    query = String("today=") + today;
  }
  DynamicJsonDocument response(API_JSON_DOC_SIZE);
  if (!perform_request("GET", "/projects", query.c_str(), nullptr, true, &response)) {
    return -1;
  }
  return workspace_utils::parse_projects(response.as<JsonVariantConst>(), out, max_items);
}

bool ApiDocumentStore::create_project(const char* title, char* out_name, size_t out_name_size) {
  DynamicJsonDocument request(512);
  request["title"] = title;
  String payload;
  serializeJson(request, payload);

  DynamicJsonDocument response(2048);
  if (!perform_request("POST", "/projects", nullptr, payload.c_str(), true, &response)) {
    return false;
  }
  api_document_store_utils::safe_copy(out_name, out_name_size, response["name"] | "");
  return out_name == nullptr || out_name[0] != '\0';
}

int ApiDocumentStore::list_tasks(const char* project,
                                 const char* due_filter,
                                 const char* today,
                                 task_item_t* out,
                                 int max_items,
                                 int* out_total) {
  if (out_total) {
    *out_total = 0;
  }
  if (!out || max_items <= 0) {
    return 0;
  }
  String query = String("limit=") + max_items;
  if (project && project[0] != '\0') {
    query += "&project=";
    query += percent_encode(project);
  }
  if (due_filter && due_filter[0] != '\0') {
    query += "&due=";
    query += due_filter;
  }
  if (workspace_utils::is_valid_date(today)) {
    query += "&today=";
    query += today;
  }

  DynamicJsonDocument response(API_JSON_DOC_SIZE);
  if (!perform_request("GET", "/tasks", query.c_str(), nullptr, true, &response)) {
    return -1;
  }
  return workspace_utils::parse_tasks(response.as<JsonVariantConst>(), out, max_items, out_total);
}

bool ApiDocumentStore::add_task(const char* project, const task_input_t& task) {
  DynamicJsonDocument request(512);
  request["project"] = project;
  request["text"] = task.text;
  if (task.due[0] != '\0') {
    request["due"] = task.due;
  }
  if (task.time[0] != '\0') {
    request["time"] = task.time;
  }
  if (task.priority > 0) {
    request["priority"] = task.priority;
  }
  String payload;
  serializeJson(request, payload);
  return perform_request("POST", "/tasks", nullptr, payload.c_str(), true, nullptr);
}

bool ApiDocumentStore::set_task_done(const char* file, uint16_t line, bool done) {
  DynamicJsonDocument request(512);
  request["file"] = file;
  request["line"] = line;
  request["done"] = done;
  String payload;
  serializeJson(request, payload);
  return perform_request("POST", "/tasks/toggle", nullptr, payload.c_str(), true, nullptr);
}

bool ApiDocumentStore::append_line(const char* id, const char* text) {
  DynamicJsonDocument request(1024);
  request["id"] = id;
  request["text"] = text;
  String payload;
  serializeJson(request, payload);
  return perform_request("POST", "/files/append", nullptr, payload.c_str(), true, nullptr);
}

int ApiDocumentStore::search(const char* query_text, search_hit_t* out, int max_items) {
  if (!out || max_items <= 0 || !query_text || query_text[0] == '\0') {
    return 0;
  }
  String query = String("limit=") + max_items + "&q=" + percent_encode(query_text);
  DynamicJsonDocument response(API_JSON_DOC_SIZE);
  if (!perform_request("GET", "/search", query.c_str(), nullptr, true, &response)) {
    return -1;
  }
  return workspace_utils::parse_search_hits(response.as<JsonVariantConst>(), out, max_items);
}

bool ApiDocumentStore::is_available() const {
  return available_;
}

const char* ApiDocumentStore::last_error() const {
  return last_error_;
}
