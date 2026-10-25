#include "chat_service.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <stdio.h>
#include <string.h>

#include "app/lang.h"
#include "platform/app_log.h"
#include "tls_roots.h"

namespace {
constexpr uint16_t CHAT_HTTP_TIMEOUT_MS = 15000;
// Reasoning models may stay silent for a while, but they send deltas while thinking.
constexpr uint32_t CHAT_STREAM_IDLE_TIMEOUT_MS = 30000;
constexpr uint32_t CHAT_STREAM_POLL_MS = 10;
constexpr size_t CHAT_STREAM_READ_SIZE = 256;
constexpr size_t CHAT_MODELS_DOC_SIZE = 16384;
constexpr const char* CHAT_MODEL_OWNER = "digitalocean";
constexpr size_t CHAT_REQUEST_DOC_SIZE = 8192;
constexpr size_t CHAT_RESPONSE_DOC_SIZE = 16384;
constexpr uint16_t CHAT_MAX_TOKENS = 192;
constexpr uint16_t CHAT_RETRY_MAX_TOKENS = 256;
constexpr const char* CHAT_REASONING_EFFORT = "low";
// Agent mode request: only the nodes, the strings are referenced (the history is ~4 KB).
constexpr size_t CHAT_AGENT_DOC_SIZE = 1024;
constexpr const char* ASSISTANT_PATH = "/assistant";
constexpr const char* ASSISTANT_CONFIRM_PATH = "/assistant/confirm";
constexpr const char* CHAT_SYSTEM_PROMPT =
    TR("Tu reponds en francais, de facon concise, claire et lisible sur un Minitel 40 colonnes. "
       "Evite le markdown complexe, les longues listes et les phrases inutiles.",
       "You answer in English, concisely, clearly and readably on a 40-column Minitel. "
       "Avoid complex markdown, long lists and needless sentences.");
constexpr const char* CHAT_RETRY_SYSTEM_PROMPT =
    TR("Donne maintenant uniquement la reponse finale, sans developper ton raisonnement. "
       "La reponse doit etre breve et directe.",
       "Now give only the final answer, without spelling out your reasoning. "
       "The answer must be short and direct.");

#ifndef CHAT_SERVICE_ALLOW_INSECURE_TLS
#define CHAT_SERVICE_ALLOW_INSECURE_TLS 0
#endif

// Decoder state of the reply being streamed (1.3 KB): kept off the network task stack.
// Only the task running stream_message() uses it.
chat_stream_t g_stream;

void configure_tls_client(WiFiClientSecure* client) {
  if (!client) {
    return;
  }

  client->setHandshakeTimeout(TLS_HANDSHAKE_TIMEOUT_S);
#if CHAT_SERVICE_ALLOW_INSECURE_TLS
  client->setInsecure();
#else
  client->setCACert(TLS_ROOT_CERTS);
#endif
}

// Only the gpt-oss models are known to accept "reasoning_effort"; other providers may
// reject the unknown field with a 400.
bool model_accepts_reasoning_effort(const char* model) {
  return model && strncmp(model, "openai-gpt-oss", 14) == 0;
}

bool has_non_empty_text_field(const JsonVariantConst& value) {
  if (value.is<const char*>()) {
    const char* text = value.as<const char*>();
    return text && text[0] != '\0';
  }

  if (value.is<JsonArrayConst>()) {
    for (JsonVariantConst item : value.as<JsonArrayConst>()) {
      if (item.is<const char*>()) {
        const char* text = item.as<const char*>();
        if (text && text[0] != '\0') {
          return true;
        }
        continue;
      }

      if (!item.is<JsonObjectConst>()) {
        continue;
      }

      JsonVariantConst text = item["text"];
      if (text.is<const char*>()) {
        const char* value_text = text.as<const char*>();
        if (value_text && value_text[0] != '\0') {
          return true;
        }
      }
    }
  }

  return false;
}

// Request body: system prompt(s), the remembered exchanges, then the new prompt.
void build_payload(const chat_history_t& history,
                   const char* prompt,
                   const char* model,
                   bool retry_mode,
                   String* out) {
  DynamicJsonDocument request(CHAT_REQUEST_DOC_SIZE);
  request["model"] = model;
  request["max_tokens"] = retry_mode ? CHAT_RETRY_MAX_TOKENS : CHAT_MAX_TOKENS;
  request["stream"] = true;
  if (model_accepts_reasoning_effort(model)) {
    request["reasoning_effort"] = CHAT_REASONING_EFFORT;
  }

  JsonArray messages = request.createNestedArray("messages");

  JsonObject system_message = messages.createNestedObject();
  system_message["role"] = "system";
  system_message["content"] = CHAT_SYSTEM_PROMPT;

  if (retry_mode) {
    JsonObject retry_message = messages.createNestedObject();
    retry_message["role"] = "system";
    retry_message["content"] = CHAT_RETRY_SYSTEM_PROMPT;
  }

  for (uint8_t i = 0; i < history.count; ++i) {
    const chat_turn_t& turn = history.turns[i];
    if (turn.user[0] != '\0') {
      JsonObject user_message = messages.createNestedObject();
      user_message["role"] = "user";
      user_message["content"] = turn.user;
    }
    if (turn.assistant[0] != '\0') {
      JsonObject assistant_message = messages.createNestedObject();
      assistant_message["role"] = "assistant";
      assistant_message["content"] = turn.assistant;
    }
  }

  JsonObject user_message = messages.createNestedObject();
  user_message["role"] = "user";
  user_message["content"] = prompt;

  serializeJson(request, *out);
}

void report_progress(const chat_stream_hooks_t& hooks) {
  if (!hooks.progress) {
    return;
  }
  chat_stage_t stage = CHAT_STAGE_CONNECTING;
  if (g_stream.text.length > g_stream.text_start) {
    stage = CHAT_STAGE_WRITING;
  } else if (g_stream.actions.length > g_stream.actions_start) {
    stage = CHAT_STAGE_ACTING;
  } else if (g_stream.reasoning) {
    stage = CHAT_STAGE_THINKING;
  }
  hooks.progress(stage, g_stream.text.length, g_stream.actions.length);
}

bool is_cancelled(const chat_stream_hooks_t& hooks) {
  return hooks.cancelled && hooks.cancelled();
}

// Reads the event stream of `http` into g_stream, begun by the caller, until the reply ends,
// the hooks cancel it or nothing comes for a while; then closes the connection. Returns
// false when it stopped for lack of data.
bool read_event_stream(HTTPClient* http, const chat_stream_hooks_t& hooks) {
  WiFiClient* body = http->getStreamPtr();
  uint8_t buffer[CHAT_STREAM_READ_SIZE];
  uint32_t last_data_ms = millis();
  size_t reported_length = g_stream.text.length;
  size_t reported_actions = g_stream.actions.length;
  bool reported_reasoning = false;
  bool timed_out = false;
  while (body && !is_cancelled(hooks)) {
    const int available = body->available();
    if (available <= 0) {
      if (!body->connected()) {
        break;  // closed by the server: the reply is over
      }
      if (millis() - last_data_ms > CHAT_STREAM_IDLE_TIMEOUT_MS) {
        timed_out = true;
        break;
      }
      delay(CHAT_STREAM_POLL_MS);
      continue;
    }
    const size_t wanted = static_cast<size_t>(available) < sizeof(buffer) ? static_cast<size_t>(available)
                                                                           : sizeof(buffer);
    const int read = body->read(buffer, wanted);
    if (read <= 0) {
      delay(CHAT_STREAM_POLL_MS);
      continue;
    }
    last_data_ms = millis();
    const bool more = chat_service_utils::stream_feed(&g_stream, reinterpret_cast<const char*>(buffer),
                                                      static_cast<size_t>(read));
    if (g_stream.text.length != reported_length || g_stream.actions.length != reported_actions ||
        g_stream.reasoning != reported_reasoning) {
      reported_length = g_stream.text.length;
      reported_actions = g_stream.actions.length;
      reported_reasoning = g_stream.reasoning;
      report_progress(hooks);
    }
    if (!more) {
      break;
    }
  }
  http->end();
  chat_service_utils::stream_finish(&g_stream);

  if (g_stream.chunked.failed) {
    app_log_warn("chat", "malformed chunked body");
  }
  if (g_stream.events.dropped > 0) {
    app_log_warn("chat", "events too large dropped=%u", static_cast<unsigned>(g_stream.events.dropped));
  }
  return !timed_out;
}

// The server ignored "stream": read the completion as a whole, into the same decoder state.
bool read_whole_reply(const String& body, char* out_reply, size_t out_reply_size) {
  chat_service_utils::stream_begin(&g_stream, out_reply, out_reply_size, false);
  DynamicJsonDocument response(CHAT_RESPONSE_DOC_SIZE);
  const DeserializationError json_error = deserializeJson(response, body);
  if (json_error) {
    app_log_warn("chat", "json error=%s body=%s", json_error.c_str(), body.c_str());
    return false;
  }

  JsonVariantConst root = response.as<JsonVariantConst>();
  JsonVariantConst choice = root["choices"][0];
  char raw[CHAT_MAX_REPLY_LENGTH + 1];
  bool raw_truncated = false;
  if (chat_service_utils::parse_chat_response(root, raw, sizeof(raw), &raw_truncated)) {
    chat_service_utils::text_builder_append(&g_stream.text, raw);
  }
  const char* finish_reason = choice["finish_reason"];
  g_stream.length_limited = raw_truncated || (finish_reason && strcmp(finish_reason, "length") == 0);
  g_stream.reasoning = has_non_empty_text_field(choice["message"]["reasoning_content"]);
  g_stream.finished = true;
  chat_service_utils::stream_finish(&g_stream);
  return true;
}

}  // namespace

ChatService::ChatService() {
  base_url_[0] = '\0';
  api_key_[0] = '\0';
  model_[0] = '\0';
  api_url_[0] = '\0';
  api_token_[0] = '\0';
  last_error_[0] = '\0';
}

void ChatService::configure_assistant(const char* api_url, const char* api_token) {
  chat_service_utils::safe_copy(api_url_, sizeof(api_url_), api_url);
  chat_service_utils::safe_copy(api_token_, sizeof(api_token_), api_token);
  chat_service_utils::trim_in_place(api_url_);
  chat_service_utils::trim_in_place(api_token_);
  while (api_url_[0] != '\0' && api_url_[strlen(api_url_) - 1] == '/') {
    api_url_[strlen(api_url_) - 1] = '\0';
  }
}

void ChatService::configure(const char* base_url, const char* api_key, const char* model) {
  chat_service_utils::safe_copy(base_url_, sizeof(base_url_), base_url);
  chat_service_utils::safe_copy(api_key_, sizeof(api_key_), api_key);
  chat_service_utils::safe_copy(model_, sizeof(model_), model);

  chat_service_utils::trim_in_place(base_url_);
  chat_service_utils::trim_in_place(api_key_);
  chat_service_utils::trim_in_place(model_);

  while (base_url_[0] != '\0' && base_url_[strlen(base_url_) - 1] == '/') {
    base_url_[strlen(base_url_) - 1] = '\0';
  }

  last_error_[0] = '\0';
  app_log_info("chat",
               "configure url=\"%s\" key=%s model=\"%s\"",
               base_url_[0] ? base_url_ : "<empty>",
               api_key_[0] ? "set" : "empty",
               model_[0] ? model_ : "<empty>");
}

bool ChatService::is_configured() const {
  return url_uses_tls(base_url_) && api_key_[0] != '\0' && model_[0] != '\0';
}

void ChatService::clear_error() {
  last_error_[0] = '\0';
}

void ChatService::set_error(const char* message) {
  chat_service_utils::safe_copy(last_error_, sizeof(last_error_), message);
}

void ChatService::set_http_error(int status_code, const char* response_body) {
  char message[sizeof(last_error_)];
  message[0] = '\0';

  switch (status_code) {
    case 400:
      snprintf(message, sizeof(message), TR("IA 400 requete invalide", "AI 400 invalid request"));
      break;
    case 401:
    case 403:
      snprintf(message, sizeof(message), TR("IA %d cle invalide", "AI %d invalid key"), status_code);
      break;
    case 404:
      snprintf(message, sizeof(message), TR("IA 404 endpoint absent", "AI 404 no such endpoint"));
      break;
    case 408:
      snprintf(message, sizeof(message), TR("IA timeout", "AI timeout"));
      break;
    case 429:
      snprintf(message, sizeof(message), TR("IA quota atteint", "AI quota reached"));
      break;
    default:
      if (status_code >= 500) {
        snprintf(message, sizeof(message), TR("IA %d erreur serveur", "AI %d server error"), status_code);
      } else if (status_code > 0) {
        snprintf(message, sizeof(message), TR("IA HTTP %d", "AI HTTP %d"), status_code);
      } else {
        String error = HTTPClient::errorToString(status_code);
        snprintf(message,
                 sizeof(message),
                 TR("IA %s", "AI %s"),
                 error.length() > 0 ? error.c_str() : TR("injoignable", "unreachable"));
      }
      break;
  }

  if (response_body && response_body[0] != '\0') {
    app_log_warn("chat", "http error status=%d body=%s", status_code, response_body);
  }
  set_error(message);
}

// Agent mode: the error comes from Bibliotek, the model's own errors arrive in the stream.
void ChatService::set_assistant_http_error(bool confirmation, int status_code, const char* response_body) {
  char message[sizeof(last_error_)];
  switch (status_code) {
    case 400:
      snprintf(message, sizeof(message), TR("Assistant: requete invalide", "Assistant: invalid request"));
      break;
    case 401:
    case 403:
      snprintf(message, sizeof(message), TR("Docs token invalide", "Invalid Docs token"));
      break;
    case 404:
      // A confirmation answered too late, or a Bibliotek older than the assistant.
      snprintf(message, sizeof(message),
               confirmation ? TR("Confirmation expiree", "Confirmation expired")
                            : TR("Serveur sans assistant: a mettre a jour", "Server has no assistant: update it"));
      break;
    case 301:
    case 302:
    case 307:
    case 308:
      snprintf(message, sizeof(message), TR("API redirigee: utilise https://", "API redirected: use https://"));
      break;
    default:
      if (status_code >= 500) {
        snprintf(message, sizeof(message), TR("Bibliotek %d erreur serveur", "Bibliotek %d server error"), status_code);
      } else if (status_code > 0) {
        snprintf(message, sizeof(message), "Bibliotek HTTP %d", status_code);
      } else {
        String error = HTTPClient::errorToString(status_code);
        snprintf(message,
                 sizeof(message),
                 "Bibliotek %s",
                 error.length() > 0 ? error.c_str() : TR("injoignable", "unreachable"));
      }
      break;
  }

  if (response_body && response_body[0] != '\0') {
    app_log_warn("chat", "assistant error status=%d body=%s", status_code, response_body);
  }
  set_error(message);
}

bool ChatService::check_ai_settings() {
  if (base_url_[0] == '\0') {
    set_error(TR("IA URL vide", "Empty AI URL"));
    return false;
  }
  if (!url_uses_tls(base_url_)) {
    set_error(TR("IA URL https requise", "AI URL must be https"));
    return false;
  }
  if (api_key_[0] == '\0') {
    set_error(TR("IA cle vide", "Empty AI key"));
    return false;
  }
  if (model_[0] == '\0') {
    set_error(TR("IA modele vide", "Empty AI model"));
    return false;
  }
  return true;
}

// The AI key goes to Bibliotek with the question: never in clear.
bool ChatService::check_assistant_settings() {
  if (api_url_[0] == '\0' || api_token_[0] == '\0') {
    set_error(TR("Assistant: Docs URL/token vides", "Assistant: empty Docs URL/token"));
    return false;
  }
  if (!url_uses_tls(api_url_)) {
    set_error(TR("Assistant: Docs URL https requise", "Assistant: Docs URL must be https"));
    return false;
  }
  if (WiFi.status() != WL_CONNECTED) {
    set_error(TR("WiFi hors ligne", "WiFi offline"));
    return false;
  }
  return true;
}

bool ChatService::stream_attempt(const chat_history_t& history,
                                 const char* prompt,
                                 bool retry_mode,
                                 char* out_reply,
                                 size_t out_reply_size,
                                 const chat_stream_hooks_t& hooks) {
  chat_service_utils::stream_begin(&g_stream, out_reply, out_reply_size, false);
  report_progress(hooks);

  String url(base_url_);
  url += "/chat/completions";
  String payload;
  build_payload(history, prompt, model_, retry_mode, &payload);
  app_log_info("chat",
               "send url=%s model=\"%s\" history=%u prompt_len=%u retry=%s",
               url.c_str(),
               model_,
               static_cast<unsigned>(history.count),
               static_cast<unsigned>(strlen(prompt)),
               retry_mode ? "yes" : "no");

  WiFiClientSecure client;
  configure_tls_client(&client);
  HTTPClient http;
  if (!http.begin(client, url)) {
    set_error(TR("IA inaccessible", "AI unreachable"));
    return false;
  }
  http.setReuse(false);  // the reply may be left unread: never keep the connection
  http.setTimeout(CHAT_HTTP_TIMEOUT_MS);
  const char* header_keys[] = {"Content-Type", "Transfer-Encoding"};
  http.collectHeaders(header_keys, 2);
  http.addHeader("Accept", "text/event-stream");
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", String("Bearer ") + api_key_);

  const int status_code = http.POST(payload);
  payload = String();  // the TLS buffers need the heap more now
  if (status_code < 200 || status_code >= 300) {
    const String body = status_code > 0 ? http.getString() : String();
    http.end();
    set_http_error(status_code, body.c_str());
    return false;
  }

  if (!http.header("Content-Type").startsWith("text/event-stream")) {
    const String body = http.getString();
    http.end();
    if (!read_whole_reply(body, out_reply, out_reply_size)) {
      set_error(TR("JSON invalide", "Invalid JSON"));
      return false;
    }
    report_progress(hooks);
    return true;
  }

  chat_service_utils::stream_begin(
      &g_stream, out_reply, out_reply_size, http.header("Transfer-Encoding").equalsIgnoreCase("chunked"));
  const bool timed_out = !read_event_stream(&http, hooks);
  if (timed_out && g_stream.text.length == 0) {
    set_error(TR("IA timeout", "AI timeout"));
    return false;
  }
  return true;
}

// POSTs `payload` to Bibliotek's assistant and decodes its stream into `reply`.
bool ChatService::agent_attempt(bool confirmation,
                                String* payload,
                                const chat_agent_reply_t& reply,
                                const chat_stream_hooks_t& hooks,
                                bool* out_truncated) {
  if (reply.confirm) {
    memset(reply.confirm, 0, sizeof(*reply.confirm));
  }
  chat_service_utils::stream_resume(&g_stream, reply.text, reply.text_size, reply.actions, reply.actions_size, false);
  report_progress(hooks);

  String url(api_url_);
  url += confirmation ? ASSISTANT_CONFIRM_PATH : ASSISTANT_PATH;
  app_log_info("chat",
               "assistant url=%s model=\"%s\" payload=%u",
               url.c_str(),
               model_,
               static_cast<unsigned>(payload->length()));

  WiFiClientSecure client;
  configure_tls_client(&client);
  HTTPClient http;
  if (!http.begin(client, url)) {
    set_error(TR("Bibliotek inaccessible", "Bibliotek unreachable"));
    return false;
  }
  http.setReuse(false);  // the reply may be left unread: never keep the connection
  http.setTimeout(CHAT_HTTP_TIMEOUT_MS);
  const char* header_keys[] = {"Content-Type", "Transfer-Encoding"};
  http.collectHeaders(header_keys, 2);
  http.addHeader("Accept", "text/event-stream");
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", String("Bearer ") + api_token_);

  const int status_code = http.POST(*payload);
  *payload = String();  // it holds the AI key, and the TLS buffers need the heap more now
  if (status_code < 200 || status_code >= 300) {
    const String body = status_code > 0 ? http.getString() : String();
    http.end();
    set_assistant_http_error(confirmation, status_code, body.c_str());
    return false;
  }
  if (!http.header("Content-Type").startsWith("text/event-stream")) {
    http.end();
    set_error(TR("Reponse assistant invalide", "Invalid assistant reply"));
    return false;
  }

  chat_service_utils::stream_resume(&g_stream,
                                    reply.text,
                                    reply.text_size,
                                    reply.actions,
                                    reply.actions_size,
                                    http.header("Transfer-Encoding").equalsIgnoreCase("chunked"));
  const bool timed_out = !read_event_stream(&http, hooks);
  if (reply.confirm) {
    *reply.confirm = g_stream.confirm;
  }
  if (is_cancelled(hooks)) {
    set_error(TR("Requete IA annulee", "AI request cancelled"));
    return false;
  }
  if (g_stream.failed) {
    // The assistant's own message (model errors), else a cut stream.
    set_error(g_stream.error[0] != '\0' ? g_stream.error : TR("Reponse IA interrompue", "AI reply interrupted"));
    return false;
  }
  if (g_stream.confirm.id[0] != '\0') {
    clear_error();
    app_log_info("chat", "assistant waits for a confirmation");
    return true;
  }
  if (timed_out && g_stream.text.length == g_stream.text_start) {
    set_error(TR("IA timeout", "AI timeout"));
    return false;
  }
  if (g_stream.text.length == 0 && g_stream.actions.length == 0) {
    set_error(TR("Reponse vide", "Empty reply"));
    return false;
  }
  if (out_truncated) {
    *out_truncated = g_stream.text.truncated || g_stream.length_limited || !g_stream.finished;
  }
  clear_error();
  app_log_info("chat",
               "assistant reply len=%u actions=%u truncated=%s",
               static_cast<unsigned>(g_stream.text.length),
               static_cast<unsigned>(g_stream.actions.length),
               out_truncated && *out_truncated ? "yes" : "no");
  return true;
}

bool ChatService::stream_agent(const chat_history_t& history,
                               const char* prompt,
                               const char* now,
                               const chat_agent_reply_t& reply,
                               const chat_stream_hooks_t& hooks,
                               bool* out_truncated) {
  if (out_truncated) {
    *out_truncated = false;
  }
  if (!reply.text || reply.text_size == 0) {
    return false;
  }
  if (!check_ai_settings() || !check_assistant_settings()) {
    return false;
  }

  char normalized_prompt[CHAT_MAX_PROMPT_LENGTH + 1];
  if (!chat_service_utils::normalize_text(prompt, normalized_prompt, sizeof(normalized_prompt), nullptr)) {
    set_error(TR("Prompt vide", "Empty prompt"));
    return false;
  }

  String payload;
  {
    DynamicJsonDocument request(CHAT_AGENT_DOC_SIZE);
    chat_agent_settings_t settings;
    settings.ai_url = base_url_;
    settings.ai_key = api_key_;
    settings.model = model_;
    settings.now = now ? now : "";
    chat_service_utils::build_agent_request(request, history, normalized_prompt, settings);
    if (request.overflowed()) {
      set_error(TR("Requete assistant trop grande", "Assistant request too large"));
      return false;
    }
    serializeJson(request, payload);
  }
  return agent_attempt(false, &payload, reply, hooks, out_truncated);
}

bool ChatService::answer_confirmation(const char* id,
                                      bool accept,
                                      const chat_agent_reply_t& reply,
                                      const chat_stream_hooks_t& hooks,
                                      bool* out_truncated) {
  if (out_truncated) {
    *out_truncated = false;
  }
  if (!reply.text || reply.text_size == 0 || !id || id[0] == '\0') {
    return false;
  }
  if (!check_assistant_settings()) {
    return false;
  }

  StaticJsonDocument<128> request;
  request["id"] = id;
  request["accept"] = accept;
  String payload;
  serializeJson(request, payload);
  return agent_attempt(true, &payload, reply, hooks, out_truncated);
}

bool ChatService::stream_message(const chat_history_t& history,
                                 const char* prompt,
                                 char* out_reply,
                                 size_t out_reply_size,
                                 const chat_stream_hooks_t& hooks,
                                 bool* out_truncated) {
  if (out_truncated) {
    *out_truncated = false;
  }
  if (!out_reply || out_reply_size == 0) {
    return false;
  }
  out_reply[0] = '\0';

  if (!check_ai_settings()) {
    return false;
  }
  if (WiFi.status() != WL_CONNECTED) {
    set_error(TR("WiFi hors ligne", "WiFi offline"));
    return false;
  }

  char normalized_prompt[CHAT_MAX_PROMPT_LENGTH + 1];
  bool prompt_truncated = false;
  if (!chat_service_utils::normalize_text(
          prompt, normalized_prompt, sizeof(normalized_prompt), &prompt_truncated)) {
    set_error(TR("Prompt vide", "Empty prompt"));
    return false;
  }
  (void)prompt_truncated;

  if (!stream_attempt(history, normalized_prompt, false, out_reply, out_reply_size, hooks)) {
    return false;
  }
  // Reasoning models may spend every token thinking: ask again for the answer only.
  if (g_stream.text.length == 0 && !is_cancelled(hooks) && (g_stream.reasoning || g_stream.length_limited)) {
    app_log_warn("chat", "empty content with reasoning -> retry");
    if (!stream_attempt(history, normalized_prompt, true, out_reply, out_reply_size, hooks)) {
      return false;
    }
    if (g_stream.text.length == 0 && !is_cancelled(hooks)) {
      set_error(TR("Reponse incomplete", "Incomplete reply"));
      return false;
    }
  }
  if (is_cancelled(hooks)) {
    set_error(TR("Requete IA annulee", "AI request cancelled"));
    return false;
  }
  if (g_stream.text.length == 0) {
    set_error(g_stream.failed ? TR("IA erreur pendant la reponse", "AI error during the reply")
                              : TR("Reponse vide", "Empty reply"));
    return false;
  }

  if (out_truncated) {
    // Cut by the token limit, by the buffer, or by a connection closed before the end.
    *out_truncated = g_stream.text.truncated || g_stream.length_limited || !g_stream.finished;
  }
  if (g_stream.failed) {
    set_error(TR("Reponse IA interrompue", "AI reply interrupted"));  // what arrived stays in out_reply
    return false;
  }
  clear_error();
  app_log_info("chat",
               "reply len=%u truncated=%s",
               static_cast<unsigned>(g_stream.text.length),
               out_truncated && *out_truncated ? "yes" : "no");
  return true;
}

int ChatService::list_models(char out[][CHAT_MAX_MODEL_ID_LENGTH + 1], int max_items) {
  if (!out || max_items <= 0) {
    return 0;
  }
  if (!url_uses_tls(base_url_)) {
    set_error(TR("IA URL https requise", "AI URL must be https"));
    return -1;
  }
  if (api_key_[0] == '\0') {
    set_error(TR("IA cle vide", "Empty AI key"));
    return -1;
  }
  if (WiFi.status() != WL_CONNECTED) {
    set_error(TR("WiFi hors ligne", "WiFi offline"));
    return -1;
  }

  String url(base_url_);
  url += "/models";

  WiFiClientSecure client;
  configure_tls_client(&client);
  HTTPClient http;
  if (!http.begin(client, url)) {
    set_error(TR("IA inaccessible", "AI unreachable"));
    return -1;
  }
  http.setTimeout(CHAT_HTTP_TIMEOUT_MS);
  http.addHeader("Accept", "application/json");
  http.addHeader("Authorization", String("Bearer ") + api_key_);
  const int status_code = http.GET();
  const String raw_response = status_code > 0 ? http.getString() : String();
  http.end();

  if (status_code < 200 || status_code >= 300) {
    set_http_error(status_code, raw_response.c_str());
    return -1;
  }

  // The full listing is ~10 KB: keep only the fields the model filter needs.
  StaticJsonDocument<256> filter;
  filter["data"][0]["id"] = true;
  filter["data"][0]["owned_by"] = true;
  filter["data"][0]["context_length"] = true;
  filter["data"][0]["max_output_tokens"] = true;
  DynamicJsonDocument response(CHAT_MODELS_DOC_SIZE);
  const DeserializationError json_error =
      deserializeJson(response, raw_response, DeserializationOption::Filter(filter));
  if (json_error) {
    app_log_warn("chat", "models json error=%s", json_error.c_str());
    set_error(TR("JSON modeles invalide", "Invalid models JSON"));
    return -1;
  }

  const int count = chat_service_utils::collect_chat_models(
      response.as<JsonVariantConst>(), CHAT_MODEL_OWNER, out, max_items);
  clear_error();
  app_log_info("chat", "models owner=%s count=%d", CHAT_MODEL_OWNER, count);
  return count;
}

const char* ChatService::last_error() const {
  return last_error_;
}
