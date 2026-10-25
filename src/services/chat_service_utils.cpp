#include "chat_service_utils.h"

#include <ctype.h>
#include <string.h>
#include <strings.h>

namespace chat_service_utils {
namespace {

void append_char(char ch, char* out, size_t out_size, size_t* out_index, bool* truncated) {
  if (!out || !out_index || out_size == 0) {
    return;
  }

  if (*out_index + 1 < out_size) {
    out[*out_index] = ch;
    ++(*out_index);
    out[*out_index] = '\0';
    return;
  }

  if (truncated) {
    *truncated = true;
  }
}

bool append_string(const char* text,
                   char* out,
                   size_t out_size,
                   size_t* out_index,
                   bool* truncated) {
  if (!text) {
    return false;
  }

  bool any = false;
  for (size_t i = 0; text[i] != '\0'; ++i) {
    append_char(text[i], out, out_size, out_index, truncated);
    any = true;
  }
  return any;
}

bool append_replacement(const char* replacement,
                        char* out,
                        size_t out_size,
                        size_t* out_index,
                        bool* truncated) {
  if (!replacement) {
    return false;
  }

  bool any = false;
  for (size_t i = 0; replacement[i] != '\0'; ++i) {
    append_char(replacement[i], out, out_size, out_index, truncated);
    any = true;
  }
  return any;
}

bool decode_utf8_code_point(const char* input, uint32_t* out_code_point, size_t* out_length) {
  if (!input || !out_code_point || !out_length) {
    return false;
  }

  const uint8_t first = static_cast<uint8_t>(input[0]);
  if (first < 0x80) {
    *out_code_point = first;
    *out_length = 1;
    return true;
  }

  if ((first & 0xE0) == 0xC0) {
    const uint8_t second = static_cast<uint8_t>(input[1]);
    if ((second & 0xC0) != 0x80) {
      return false;
    }
    *out_code_point = static_cast<uint32_t>(first & 0x1F) << 6 |
                      static_cast<uint32_t>(second & 0x3F);
    *out_length = 2;
    return true;
  }

  // Check each continuation byte before reading the next one: a sequence cut by the end of
  // the string must not read past its terminator.
  if ((first & 0xF0) == 0xE0) {
    const uint8_t second = static_cast<uint8_t>(input[1]);
    if ((second & 0xC0) != 0x80) {
      return false;
    }
    const uint8_t third = static_cast<uint8_t>(input[2]);
    if ((third & 0xC0) != 0x80) {
      return false;
    }
    *out_code_point = static_cast<uint32_t>(first & 0x0F) << 12 |
                      static_cast<uint32_t>(second & 0x3F) << 6 |
                      static_cast<uint32_t>(third & 0x3F);
    *out_length = 3;
    return true;
  }

  if ((first & 0xF8) == 0xF0) {
    const uint8_t second = static_cast<uint8_t>(input[1]);
    if ((second & 0xC0) != 0x80) {
      return false;
    }
    const uint8_t third = static_cast<uint8_t>(input[2]);
    if ((third & 0xC0) != 0x80) {
      return false;
    }
    const uint8_t fourth = static_cast<uint8_t>(input[3]);
    if ((fourth & 0xC0) != 0x80) {
      return false;
    }
    *out_code_point = static_cast<uint32_t>(first & 0x07) << 18 |
                      static_cast<uint32_t>(second & 0x3F) << 12 |
                      static_cast<uint32_t>(third & 0x3F) << 6 |
                      static_cast<uint32_t>(fourth & 0x3F);
    *out_length = 4;
    return true;
  }

  return false;
}

const char* transliterate_code_point(uint32_t code_point) {
  switch (code_point) {
    case 0x00A0:
    case 0x2000:
    case 0x2001:
    case 0x2002:
    case 0x2003:
    case 0x2004:
    case 0x2005:
    case 0x2006:
    case 0x2007:
    case 0x2008:
    case 0x2009:
    case 0x200A:
    case 0x202F:
    case 0x205F:
      return " ";
    case 0x0300:
    case 0x0301:
    case 0x0302:
    case 0x0303:
    case 0x0304:
    case 0x0306:
    case 0x0307:
    case 0x0308:
    case 0x030A:
    case 0x030B:
    case 0x030C:
    case 0x0327:
      return "";
    case 0x00AB:
    case 0x00BB:
    case 0x201C:
    case 0x201D:
    case 0x201E:
      return "\"";
    case 0x2018:
    case 0x2019:
    case 0x2032:
      return "'";
    case 0x2010:
    case 0x2011:
    case 0x2012:
    case 0x2013:
    case 0x2014:
    case 0x2212:
      return "-";
    case 0x2022:
      return "*";
    case 0x2026:
      return "...";
    case 0x20AC:
      return "EUR";
    case 0x0152:
      return "OE";
    case 0x0153:
      return "oe";
    case 0x00C0:
    case 0x00C1:
    case 0x00C2:
    case 0x00C3:
    case 0x00C4:
    case 0x00C5:
      return "A";
    case 0x00C6:
      return "AE";
    case 0x00C7:
      return "C";
    case 0x00C8:
    case 0x00C9:
    case 0x00CA:
    case 0x00CB:
      return "E";
    case 0x00CC:
    case 0x00CD:
    case 0x00CE:
    case 0x00CF:
      return "I";
    case 0x00D1:
      return "N";
    case 0x00D2:
    case 0x00D3:
    case 0x00D4:
    case 0x00D5:
    case 0x00D6:
    case 0x00D8:
      return "O";
    case 0x00D9:
    case 0x00DA:
    case 0x00DB:
    case 0x00DC:
      return "U";
    case 0x00DD:
      return "Y";
    case 0x00DF:
      return "ss";
    case 0x00E0:
    case 0x00E1:
    case 0x00E2:
    case 0x00E3:
    case 0x00E4:
    case 0x00E5:
      return "a";
    case 0x00E6:
      return "ae";
    case 0x00E7:
      return "c";
    case 0x00E8:
    case 0x00E9:
    case 0x00EA:
    case 0x00EB:
      return "e";
    case 0x00EC:
    case 0x00ED:
    case 0x00EE:
    case 0x00EF:
      return "i";
    case 0x00F1:
      return "n";
    case 0x00F2:
    case 0x00F3:
    case 0x00F4:
    case 0x00F5:
    case 0x00F6:
    case 0x00F8:
      return "o";
    case 0x00F9:
    case 0x00FA:
    case 0x00FB:
    case 0x00FC:
      return "u";
    case 0x00FD:
    case 0x00FF:
      return "y";
    default:
      return nullptr;
  }
}

size_t append_sanitized_fragment(const char* input,
                                 char* out,
                                 size_t out_size,
                                 size_t* out_index,
                                 bool* truncated) {
  if (!input || input[0] == '\0') {
    return 0;
  }

  const uint8_t first = static_cast<uint8_t>(input[0]);
  if (first < 0x80) {
    char ch = static_cast<char>(first);
    if (ch >= 0x20 && ch <= 0x7E) {
      append_char(ch, out, out_size, out_index, truncated);
    } else {
      append_char('?', out, out_size, out_index, truncated);
    }
    return 1;
  }

  uint32_t code_point = 0;
  size_t sequence_length = 0;
  if (decode_utf8_code_point(input, &code_point, &sequence_length)) {
    const char* replacement = transliterate_code_point(code_point);
    if (replacement) {
      append_replacement(replacement, out, out_size, out_index, truncated);
    } else {
      append_char('?', out, out_size, out_index, truncated);
    }
    return sequence_length;
  }

  append_char('?', out, out_size, out_index, truncated);
  return 1;
}

bool append_content_variant(const JsonVariantConst& content,
                            char* out,
                            size_t out_size,
                            size_t* out_index,
                            bool* truncated) {
  if (content.is<const char*>()) {
    return append_string(content.as<const char*>(), out, out_size, out_index, truncated);
  }

  if (content.is<JsonArrayConst>()) {
    bool any = false;
    for (JsonVariantConst item : content.as<JsonArrayConst>()) {
      if (item.is<const char*>()) {
        any = append_string(item.as<const char*>(), out, out_size, out_index, truncated) || any;
        continue;
      }
      if (!item.is<JsonObjectConst>()) {
        continue;
      }

      JsonVariantConst text = item["text"];
      if (text.is<const char*>()) {
        any = append_string(text.as<const char*>(), out, out_size, out_index, truncated) || any;
      }
    }
    return any;
  }

  return false;
}

// Sized for the agent mode's events too (action, confirm), with 64-bit slots in native tests.
constexpr size_t STREAM_FILTER_DOC_SIZE = 512;
constexpr size_t STREAM_DELTA_DOC_SIZE = 640;

bool is_non_empty_string(const JsonVariantConst& value) {
  const char* text = value.as<const char*>();
  return text && text[0] != '\0';
}

// Writes a visible character, preceded by the whitespace held back since the last one.
void builder_emit_visible(chat_text_builder_t* builder, char ch) {
  if (builder->truncated) {
    return;
  }
  const size_t pending = builder->length > 0 ? builder->pending_length : 0;
  if (builder->length + pending + 1 >= builder->out_size) {
    builder->truncated = true;
    return;
  }
  for (size_t i = 0; i < pending; ++i) {
    builder->out[builder->length++] = builder->pending[i];
  }
  builder->pending_length = 0;
  builder->out[builder->length++] = ch;
  builder->out[builder->length] = '\0';
}

void builder_emit_space(chat_text_builder_t* builder) {
  if (builder->truncated || builder->length == 0) {
    return;  // leading whitespace is dropped
  }
  if (builder->pending_length < CHAT_MAX_PENDING_SPACES) {
    builder->pending[builder->pending_length++] = ' ';
  }
}

void builder_emit_newline(chat_text_builder_t* builder) {
  if (builder->truncated || builder->length == 0) {
    return;
  }
  if (builder->pending_length > 0 && builder->pending[builder->pending_length - 1] == '\n') {
    return;  // no blank lines on a 24-row screen
  }
  if (builder->pending_length < CHAT_MAX_PENDING_SPACES) {
    builder->pending[builder->pending_length++] = '\n';
  } else {
    builder->pending[CHAT_MAX_PENDING_SPACES - 1] = '\n';
  }
}

void builder_emit_replacement(chat_text_builder_t* builder, const char* replacement) {
  for (size_t i = 0; replacement[i] != '\0'; ++i) {
    if (replacement[i] == ' ') {
      builder_emit_space(builder);
    } else {
      builder_emit_visible(builder, replacement[i]);
    }
  }
}

void builder_flush_utf8(chat_text_builder_t* builder) {
  builder->utf8[builder->utf8_length] = '\0';
  uint32_t code_point = 0;
  size_t sequence_length = 0;
  const char* replacement = nullptr;
  if (decode_utf8_code_point(builder->utf8, &code_point, &sequence_length)) {
    replacement = transliterate_code_point(code_point);
  }
  builder->utf8_length = 0;
  builder->utf8_expected = 0;
  builder_emit_replacement(builder, replacement ? replacement : "?");
}

void builder_feed_byte(chat_text_builder_t* builder, uint8_t byte) {
  if (builder->utf8_expected > 0) {
    if ((byte & 0xC0) == 0x80) {
      builder->utf8[builder->utf8_length++] = static_cast<char>(byte);
      if (builder->utf8_length == builder->utf8_expected) {
        builder_flush_utf8(builder);
      }
      return;
    }
    // The sequence broke off: mark it, then read this byte on its own.
    builder->utf8_length = 0;
    builder->utf8_expected = 0;
    builder_emit_visible(builder, '?');
  }

  if (byte == '\n' && builder->after_cr) {
    builder->after_cr = false;  // second half of CRLF
    return;
  }
  builder->after_cr = byte == '\r';

  if (byte >= 0x80) {
    uint8_t expected = 0;
    if ((byte & 0xE0) == 0xC0) {
      expected = 2;
    } else if ((byte & 0xF0) == 0xE0) {
      expected = 3;
    } else if ((byte & 0xF8) == 0xF0) {
      expected = 4;
    }
    if (expected == 0) {
      builder_emit_visible(builder, '?');
      return;
    }
    builder->utf8[0] = static_cast<char>(byte);
    builder->utf8_length = 1;
    builder->utf8_expected = expected;
    return;
  }

  if (byte == '\r' || byte == '\n') {
    builder_emit_newline(builder);
  } else if (byte == ' ' || byte == '\t') {
    builder_emit_space(builder);
  } else if (byte < 0x20 || byte == 0x7F) {
    builder_emit_visible(builder, '?');
  } else {
    builder_emit_visible(builder, static_cast<char>(byte));
  }
}

bool on_stream_event(char* data, size_t length, void* context) {
  chat_stream_t* stream = static_cast<chat_stream_t*>(context);
  if (strcmp(data, "[DONE]") == 0) {
    stream->finished = true;
    return false;
  }

  StaticJsonDocument<STREAM_FILTER_DOC_SIZE> filter;
  filter["choices"][0]["delta"]["content"] = true;
  filter["choices"][0]["delta"]["reasoning_content"] = true;
  filter["choices"][0]["delta"]["reasoning"] = true;
  filter["choices"][0]["finish_reason"] = true;
  filter["error"] = true;
  filter["action"] = true;
  filter["confirm"]["id"] = true;
  filter["confirm"]["text"] = true;

  // Zero-copy: the strings of the document point into `data`.
  StaticJsonDocument<STREAM_DELTA_DOC_SIZE> doc;
  if (deserializeJson(doc, data, length, DeserializationOption::Filter(filter))) {
    return true;  // not a JSON chunk (keep-alive, unknown event): skip it
  }

  chat_stream_delta_t delta;
  if (!parse_stream_delta(doc.as<JsonVariantConst>(), &delta)) {
    return true;
  }
  if (delta.error) {
    stream->failed = true;
    normalize_text(delta.error_message, stream->error, sizeof(stream->error), nullptr);
    return false;
  }
  if (stream->agent && delta.action) {
    lines_builder_append(&stream->actions, delta.action);
  }
  if (stream->agent && delta.confirm_id) {
    safe_copy(stream->confirm.id, sizeof(stream->confirm.id), delta.confirm_id);
    normalize_text(delta.confirm_text, stream->confirm.text, sizeof(stream->confirm.text), nullptr);
    stream->finished = true;  // the reply pauses until the user answers
    return false;
  }
  if (delta.reasoning) {
    stream->reasoning = true;
  }
  if (delta.content) {
    text_builder_append(&stream->text, delta.content);
  }
  if (delta.finish_reason) {
    stream->finished = true;
    stream->length_limited = strcmp(delta.finish_reason, "length") == 0;
  }
  return !stream->finished && !stream->text.truncated;
}

bool on_stream_body(const char* bytes, size_t length, void* context) {
  chat_stream_t* stream = static_cast<chat_stream_t*>(context);
  return sse_feed(&stream->events, bytes, length, on_stream_event, stream);
}

}  // namespace

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

void trim_in_place(char* text) {
  if (!text) {
    return;
  }

  size_t start = 0;
  while (text[start] != '\0' && isspace(static_cast<unsigned char>(text[start]))) {
    ++start;
  }

  size_t end = strlen(text);
  while (end > start && isspace(static_cast<unsigned char>(text[end - 1]))) {
    --end;
  }

  if (start > 0) {
    memmove(text, text + start, end - start);
  }
  text[end - start] = '\0';
}

bool normalize_text(const char* input, char* out, size_t out_size, bool* truncated) {
  if (truncated) {
    *truncated = false;
  }
  if (!out || out_size == 0) {
    return false;
  }

  out[0] = '\0';
  if (!input) {
    return false;
  }

  size_t out_index = 0;
  bool last_was_newline = false;
  for (size_t i = 0; input[i] != '\0';) {
    const char ch = input[i];
    if (ch == '\r') {
      if (input[i + 1] == '\n') {
        ++i;
        continue;
      }
      if (!last_was_newline || out_index == 0) {
        append_char('\n', out, out_size, &out_index, truncated);
      }
      last_was_newline = true;
      ++i;
      continue;
    }

    if (ch == '\t') {
      append_char(' ', out, out_size, &out_index, truncated);
      last_was_newline = false;
      ++i;
      continue;
    }

    if (ch == '\n') {
      if (last_was_newline && out_index > 0) {
        ++i;
        continue;
      }
      append_char('\n', out, out_size, &out_index, truncated);
      last_was_newline = true;
      ++i;
      continue;
    }

    const size_t consumed = append_sanitized_fragment(input + i, out, out_size, &out_index, truncated);
    if (consumed == 0) {
      break;
    }
    last_was_newline = false;
    i += consumed;
  }

  trim_in_place(out);
  return out[0] != '\0';
}

bool parse_chat_response(const JsonVariantConst& root, char* out, size_t out_size, bool* truncated) {
  if (truncated) {
    *truncated = false;
  }
  if (!out || out_size == 0) {
    return false;
  }

  out[0] = '\0';

  JsonVariantConst choices = root["choices"];
  if (!choices.is<JsonArrayConst>() || choices.as<JsonArrayConst>().size() == 0) {
    return false;
  }

  JsonVariantConst choice = choices[0];
  JsonVariantConst message = choice["message"];
  JsonVariantConst content = message["content"];
  if (!content) {
    content = choice["text"];
  }
  if (!content) {
    return false;
  }

  size_t out_index = 0;
  bool local_truncated = false;
  if (!append_content_variant(content, out, out_size, &out_index, &local_truncated)) {
    return false;
  }

  trim_in_place(out);
  if (out[0] == '\0') {
    return false;
  }

  if (truncated) {
    // A reply stopped by the token limit ends mid-sentence even when it fits the buffer.
    const char* finish_reason = choice["finish_reason"];
    *truncated = local_truncated || (finish_reason && strcmp(finish_reason, "length") == 0);
  }
  return true;
}

void clear_history(chat_history_t* history) {
  if (!history) {
    return;
  }
  memset(history, 0, sizeof(*history));
}

bool append_turn(chat_history_t* history,
                 const char* user,
                 const char* assistant,
                 const char* actions,
                 bool assistant_truncated) {
  if (!history) {
    return false;
  }

  chat_turn_t next_turn{};
  bool ignored_truncation = false;
  bool normalized_assistant_truncated = false;
  if (!normalize_text(user, next_turn.user, sizeof(next_turn.user), &ignored_truncation)) {
    return false;
  }
  if (!normalize_text(assistant,
                      next_turn.assistant,
                      sizeof(next_turn.assistant),
                      &normalized_assistant_truncated)) {
    return false;
  }
  safe_copy(next_turn.actions, sizeof(next_turn.actions), actions);  // built clean (lines_builder)
  next_turn.assistant_truncated = assistant_truncated || normalized_assistant_truncated;

  if (history->count < CHAT_HISTORY_MAX_TURNS) {
    history->turns[history->count++] = next_turn;
    return true;
  }

  memmove(&history->turns[0],
          &history->turns[1],
          sizeof(history->turns[0]) * static_cast<size_t>(CHAT_HISTORY_MAX_TURNS - 1));
  history->turns[CHAT_HISTORY_MAX_TURNS - 1] = next_turn;
  history->count = CHAT_HISTORY_MAX_TURNS;
  return true;
}

void build_agent_request(JsonDocument& doc,
                         const chat_history_t& history,
                         const char* prompt,
                         const chat_agent_settings_t& settings) {
  doc.clear();
  JsonObject ai = doc.createNestedObject("ai");
  ai["baseUrl"] = settings.ai_url;
  ai["apiKey"] = settings.ai_key;
  ai["model"] = settings.model;
  if (settings.now && settings.now[0] != '\0') {
    doc["now"] = settings.now;
  }

  JsonArray turns = doc.createNestedArray("history");
  for (uint8_t i = 0; i < history.count; ++i) {
    const chat_turn_t& turn = history.turns[i];
    JsonObject item = turns.createNestedObject();
    item["user"] = static_cast<const char*>(turn.user);
    item["assistant"] = static_cast<const char*>(turn.assistant);
    if (turn.actions[0] != '\0') {
      item["actions"] = static_cast<const char*>(turn.actions);
    }
  }
  doc["prompt"] = prompt;
}

// Embedding, speech, image, video and router entries have no chat context window (or a
// one-token output): they cannot hold a conversation.
constexpr uint32_t CHAT_MIN_CONTEXT_LENGTH = 8192;

int collect_chat_models(const JsonVariantConst& root,
                        const char* owner,
                        char out[][CHAT_MAX_MODEL_ID_LENGTH + 1],
                        int max_items) {
  if (!out || max_items <= 0) {
    return 0;
  }

  JsonVariantConst data = root["data"];
  if (!data.is<JsonArrayConst>()) {
    return 0;
  }

  int count = 0;
  for (JsonVariantConst model : data.as<JsonArrayConst>()) {
    if (count >= max_items) {
      break;
    }
    const char* id = model["id"];
    const char* owned_by = model["owned_by"];
    if (!id || !owned_by || (owner && strcasecmp(owned_by, owner) != 0)) {
      continue;
    }
    if (model["context_length"].as<uint32_t>() < CHAT_MIN_CONTEXT_LENGTH) {
      continue;
    }
    JsonVariantConst max_output = model["max_output_tokens"];
    if (max_output.is<uint32_t>() && max_output.as<uint32_t>() <= 1) {
      continue;
    }
    if (strlen(id) > CHAT_MAX_MODEL_ID_LENGTH) {
      continue;
    }
    safe_copy(out[count], sizeof(out[count]), id);
    ++count;
  }
  return count;
}

void text_builder_begin(chat_text_builder_t* builder, char* out, size_t out_size) {
  if (!builder) {
    return;
  }
  memset(builder, 0, sizeof(*builder));
  builder->out = out;
  builder->out_size = out ? out_size : 0;
  if (builder->out_size > 0) {
    out[0] = '\0';
  } else {
    builder->truncated = true;
  }
}

void text_builder_resume(chat_text_builder_t* builder, char* out, size_t out_size) {
  if (!builder) {
    return;
  }
  memset(builder, 0, sizeof(*builder));
  builder->out = out;
  builder->out_size = out ? out_size : 0;
  if (builder->out_size == 0) {
    builder->truncated = true;
    return;
  }
  out[out_size - 1] = '\0';
  builder->length = strlen(out);
  if (builder->length > 0) {
    builder->pending[0] = '\n';
    builder->pending_length = 1;
  }
}

void text_builder_append(chat_text_builder_t* builder, const char* fragment) {
  if (!builder || !fragment) {
    return;
  }
  for (size_t i = 0; fragment[i] != '\0' && !builder->truncated; ++i) {
    builder_feed_byte(builder, static_cast<uint8_t>(fragment[i]));
  }
}

void text_builder_finish(chat_text_builder_t* builder) {
  if (!builder) {
    return;
  }
  if (builder->utf8_expected > 0) {
    builder->utf8_length = 0;
    builder->utf8_expected = 0;
    builder_emit_visible(builder, '?');
  }
  builder->pending_length = 0;
}

void lines_builder_resume(chat_lines_builder_t* builder, char* out, size_t out_size) {
  if (!builder) {
    return;
  }
  memset(builder, 0, sizeof(*builder));
  builder->out = out;
  builder->out_size = out ? out_size : 0;
  if (builder->out_size == 0) {
    builder->full = true;
    return;
  }
  out[out_size - 1] = '\0';
  builder->length = strlen(out);
  builder->full = strcmp(out, "...") == 0 ||
                  (builder->length >= 4 && strcmp(out + builder->length - 4, "\n...") == 0);
}

void lines_builder_append(chat_lines_builder_t* builder, const char* line) {
  if (!builder || builder->full || builder->out_size == 0) {
    return;
  }
  char cleaned[CHAT_MAX_ACTION_LENGTH + 1];
  if (!normalize_text(line, cleaned, sizeof(cleaned), nullptr)) {
    return;
  }
  for (char* ch = cleaned; *ch != '\0'; ++ch) {
    if (*ch == '\n') {
      *ch = ' ';
    }
  }

  const size_t separator = builder->length > 0 ? 1 : 0;
  size_t length = strlen(cleaned);
  // A line is added only when a last "\n..." still fits after it.
  if (builder->length + separator + length + 4 >= builder->out_size) {
    builder->full = true;
    if (builder->length + separator + 3 >= builder->out_size) {
      return;
    }
    strcpy(cleaned, "...");
    length = 3;
  }
  if (separator) {
    builder->out[builder->length++] = '\n';
  }
  memcpy(builder->out + builder->length, cleaned, length);
  builder->length += length;
  builder->out[builder->length] = '\0';
}

bool parse_stream_delta(const JsonVariantConst& root, chat_stream_delta_t* out) {
  if (!out) {
    return false;
  }
  memset(out, 0, sizeof(*out));
  if (!root.is<JsonObjectConst>()) {
    return false;
  }
  JsonVariantConst error = root["error"];
  if (!error.isNull()) {
    out->error = true;
    if (is_non_empty_string(error)) {
      out->error_message = error.as<const char*>();
    } else if (is_non_empty_string(error["message"])) {
      out->error_message = error["message"].as<const char*>();
    }
    return true;
  }
  // Agent mode events (Bibliotek's assistant).
  if (is_non_empty_string(root["action"])) {
    out->action = root["action"].as<const char*>();
  }
  JsonVariantConst confirm = root["confirm"];
  if (is_non_empty_string(confirm["id"])) {
    out->confirm_id = confirm["id"].as<const char*>();
    out->confirm_text = confirm["text"].as<const char*>();
  }

  // A chunk without choices (usage report) carries nothing to show.
  JsonVariantConst choices = root["choices"];
  if (!choices.is<JsonArrayConst>() || choices.as<JsonArrayConst>().size() == 0) {
    return true;
  }
  JsonVariantConst choice = choices[0];
  JsonVariantConst delta = choice["delta"];
  if (is_non_empty_string(delta["content"])) {
    out->content = delta["content"].as<const char*>();
  }
  // DeepSeek-style "reasoning_content", vLLM/OpenRouter-style "reasoning".
  out->reasoning = is_non_empty_string(delta["reasoning_content"]) || is_non_empty_string(delta["reasoning"]);
  if (is_non_empty_string(choice["finish_reason"])) {
    out->finish_reason = choice["finish_reason"].as<const char*>();
  }
  return true;
}

void stream_begin(chat_stream_t* stream, char* out, size_t out_size, bool chunked_framing) {
  if (!stream) {
    return;
  }
  memset(stream, 0, sizeof(*stream));
  chunked_init(&stream->chunked);
  sse_init(&stream->events);
  text_builder_begin(&stream->text, out, out_size);
  stream->chunked_framing = chunked_framing;
}

void stream_resume(chat_stream_t* stream,
                   char* out,
                   size_t out_size,
                   char* actions,
                   size_t actions_size,
                   bool chunked_framing) {
  if (!stream) {
    return;
  }
  memset(stream, 0, sizeof(*stream));
  chunked_init(&stream->chunked);
  sse_init(&stream->events);
  text_builder_resume(&stream->text, out, out_size);
  stream->text_start = stream->text.length;
  lines_builder_resume(&stream->actions, actions, actions_size);
  stream->actions_start = stream->actions.length;
  stream->chunked_framing = chunked_framing;
  stream->agent = true;
}

bool stream_feed(chat_stream_t* stream, const char* bytes, size_t length) {
  if (!stream || !bytes || stream->finished || stream->failed || stream->body_ended || stream->text.truncated) {
    return false;
  }
  if (stream->chunked_framing) {
    chunked_feed(&stream->chunked, bytes, length, on_stream_body, stream);
    if (stream->chunked.failed) {
      stream->failed = true;
    }
    if (stream->chunked.done) {
      stream->body_ended = true;
    }
  } else {
    on_stream_body(bytes, length, stream);
  }
  return !stream->finished && !stream->failed && !stream->body_ended && !stream->text.truncated;
}

void stream_finish(chat_stream_t* stream) {
  if (stream) {
    text_builder_finish(&stream->text);
  }
}

}  // namespace chat_service_utils
