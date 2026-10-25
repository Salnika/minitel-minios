#ifndef SERVICES_CHAT_SERVICE_UTILS_H
#define SERVICES_CHAT_SERVICE_UTILS_H

#include <stddef.h>
#include <stdint.h>

#include <ArduinoJson.h>

#include "sse_parser.h"

constexpr uint8_t CHAT_HISTORY_MAX_TURNS = 4;
constexpr size_t CHAT_MAX_PROMPT_LENGTH = 256;
constexpr size_t CHAT_MAX_REPLY_LENGTH = 512;
constexpr size_t CHAT_MAX_ACTIONS_LENGTH = 192;
constexpr size_t CHAT_MAX_ACTION_LENGTH = 100;
constexpr size_t CHAT_MAX_CONFIRM_ID = 32;
constexpr size_t CHAT_MAX_CONFIRM_TEXT = 160;
constexpr size_t CHAT_MAX_ERROR_LENGTH = 64;
constexpr size_t CHAT_MAX_MODEL_ID_LENGTH = 48;
constexpr uint8_t CHAT_MAX_PENDING_SPACES = 8;

struct chat_turn_t {
  char user[CHAT_MAX_PROMPT_LENGTH + 1];
  char assistant[CHAT_MAX_REPLY_LENGTH + 1];
  // Agent mode: what the assistant did with the notes, one action per line.
  char actions[CHAT_MAX_ACTIONS_LENGTH + 1];
  bool assistant_truncated;
};

struct chat_history_t {
  chat_turn_t turns[CHAT_HISTORY_MAX_TURNS];
  uint8_t count;
};

// Agent mode: a tool call of the assistant waiting for the user's approval.
struct chat_confirm_t {
  char id[CHAT_MAX_CONFIRM_ID + 1];  // "" when nothing waits
  char text[CHAT_MAX_CONFIRM_TEXT + 1];
};

// What the agent mode request sends (Bibliotek's POST /assistant).
struct chat_agent_settings_t {
  const char* ai_url;
  const char* ai_key;
  const char* model;
  const char* now;  // local "YYYY-MM-DD HH:MM", "" when the clock is not set
};

// Builds a reply from streamed fragments with the cleanup of normalize_text: ASCII
// transliteration (a UTF-8 sequence may be split between fragments), no blank lines, no
// leading or trailing whitespace. Whitespace is only written once more text follows it, so
// the text written so far never changes and can be shown while it grows.
struct chat_text_builder_t {
  char* out;
  size_t out_size;
  size_t length;
  bool truncated;  // `out` is full: the rest is dropped
  char pending[CHAT_MAX_PENDING_SPACES];
  uint8_t pending_length;
  char utf8[5];
  uint8_t utf8_length;
  uint8_t utf8_expected;
  bool after_cr;
};

// Lines added whole or not at all: once one does not fit, the list ends with "...".
struct chat_lines_builder_t {
  char* out;
  size_t out_size;
  size_t length;
  bool full;
};

// One chunk of a streamed completion: {"choices":[{"delta":{...},"finish_reason":...}]}.
// The agent mode adds {"action": "..."} and {"confirm": {"id": "...", "text": "..."}}.
// Strings point into the JSON, null when absent.
struct chat_stream_delta_t {
  const char* content;        // text to show
  bool reasoning;             // reasoning text: the model is still thinking
  const char* finish_reason;  // null until the last chunk
  bool error;                 // {"error": ...} event
  const char* error_message;
  const char* action;  // a tool call ran
  const char* confirm_id;  // a tool call waits for the user's approval
  const char* confirm_text;
};

// Decodes the body of a streamed chat completion: optional chunked framing, server-sent
// events, then each chunk's delta into the reply text.
struct chat_stream_t {
  chunked_decoder_t chunked;
  bool chunked_framing;
  sse_parser_t events;
  chat_text_builder_t text;
  size_t text_start;  // what the reply held before this stream (agent mode, after a confirmation)
  bool agent;         // Bibliotek's assistant: actions and confirmations too
  chat_lines_builder_t actions;
  size_t actions_start;
  chat_confirm_t confirm;  // confirm.id set: the reply pauses for the user's approval
  char error[CHAT_MAX_ERROR_LENGTH + 1];  // message of an error event
  bool finished;        // [DONE] or a finish_reason arrived
  bool body_ended;      // the chunked body is over
  bool length_limited;  // finish_reason "length": the reply is cut
  bool reasoning;       // reasoning deltas were seen
  bool failed;          // error event or malformed framing
};

namespace chat_service_utils {

void safe_copy(char* dest, size_t dest_size, const char* src);
void trim_in_place(char* text);
bool normalize_text(const char* input, char* out, size_t out_size, bool* truncated);
bool parse_chat_response(const JsonVariantConst& root, char* out, size_t out_size, bool* truncated);
void clear_history(chat_history_t* history);
// `actions` may be null (direct mode).
bool append_turn(chat_history_t* history,
                 const char* user,
                 const char* assistant,
                 const char* actions,
                 bool assistant_truncated);
// Body of the agent mode request: the AI settings, the remembered exchanges with their
// actions, and the question. Strings are referenced, not copied: they must outlive `doc`.
void build_agent_request(JsonDocument& doc,
                         const chat_history_t& history,
                         const char* prompt,
                         const chat_agent_settings_t& settings);
// Keeps the chat models owned by `owner` from an OpenAI-style /models listing.
int collect_chat_models(const JsonVariantConst& root,
                        const char* owner,
                        char out[][CHAT_MAX_MODEL_ID_LENGTH + 1],
                        int max_items);

void text_builder_begin(chat_text_builder_t* builder, char* out, size_t out_size);
// Goes on after the text `out` already holds: what comes next starts on a new line.
void text_builder_resume(chat_text_builder_t* builder, char* out, size_t out_size);
void text_builder_append(chat_text_builder_t* builder, const char* fragment);
// End of the text: a dangling UTF-8 sequence becomes '?', trailing whitespace is dropped.
void text_builder_finish(chat_text_builder_t* builder);

// Goes on after the lines `out` already holds.
void lines_builder_resume(chat_lines_builder_t* builder, char* out, size_t out_size);
// Adds `line`, cleaned up like a reply (ASCII, on one line).
void lines_builder_append(chat_lines_builder_t* builder, const char* line);

bool parse_stream_delta(const JsonVariantConst& root, chat_stream_delta_t* out);

void stream_begin(chat_stream_t* stream, char* out, size_t out_size, bool chunked_framing);
// Agent mode: the reply and the actions go on after what `out` and `actions` hold (the
// reply resumed after a confirmation), and the agent's events are decoded too.
void stream_resume(chat_stream_t* stream,
                   char* out,
                   size_t out_size,
                   char* actions,
                   size_t actions_size,
                   bool chunked_framing);
// Feeds raw body bytes. Returns false once reading more is useless: the reply finished,
// failed, or does not fit `out` anymore.
bool stream_feed(chat_stream_t* stream, const char* bytes, size_t length);
void stream_finish(chat_stream_t* stream);

}  // namespace chat_service_utils

#endif
