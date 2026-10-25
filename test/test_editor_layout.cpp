#include <ArduinoJson.h>
#include <string.h>
#include <unity.h>

#include "app/editor_layout.h"
#include "app/input.h"
#include "app/text_wrap.h"
#include "services/chat_service_utils.h"
#include "services/net_queue.h"
#include "services/ota_utils.h"
#include "services/sse_parser.h"
#include "services/timezone_utils.h"
#include "services/wifi_utils.h"
#include "storage/api_document_store_utils.h"
#include "storage/workspace_utils.h"
#include "ui/minitel_link.h"
#include "ui/minitel_ui.h"
#include "../src/app/editor_layout.cpp"
#include "../src/app/input.cpp"
#include "../src/app/text_wrap.cpp"
#include "../src/services/chat_service_utils.cpp"
#include "../src/services/net_queue.cpp"
#include "../src/services/ota_utils.cpp"
#include "../src/services/sse_parser.cpp"
#include "../src/services/timezone_utils.cpp"
#include "../src/services/wifi_utils.cpp"
#include "../src/storage/api_document_store_utils.cpp"
#include "../src/storage/workspace_utils.cpp"
#include "../src/ui/minitel_link.cpp"
#include "../src/ui/minitel_ui.cpp"
#include "../src/lib_minitel/src/gfx/graphics_engine.c"
#include "../src/lib_minitel/src/gfx/mosaic_map.c"
#include "../src/lib_minitel/src/hal/bind.cpp"

// Only delay_ms moves the clock: timeouts run instantly.
uint32_t g_fake_ms = 0;

extern "C" uint32_t get_millis(void) {
  return g_fake_ms;
}
extern "C" void delay_ms(uint32_t ms) {
  g_fake_ms += ms;
}

// Captures what the UI sends to the Minitel so renderer tests can check the exact bytes.
uint8_t g_uart_capture[4096];
size_t g_uart_capture_len = 0;

// UART input waiting to be read.
uint8_t g_rx[512];
size_t g_rx_head = 0;
size_t g_rx_tail = 0;
bool g_rx_error = false;
uint32_t g_uart_baud = 1200;
bool g_uart_echo_filter = true;

void rx_push(uint8_t byte) {
  g_rx[g_rx_head] = byte;
  g_rx_head = (g_rx_head + 1) % sizeof(g_rx);
}

void rx_clear() {
  g_rx_head = 0;
  g_rx_tail = 0;
  g_rx_error = false;
}

// The Minitel at the other end, for the link tests: answers the Protocole commands it gets
// at its own speed. Replies are in flight until the firmware next polls the UART, and arrive
// as garbage (a receive error) when the UART has moved to another speed meanwhile.
constexpr uint8_t FAKE_SCREEN = 0;
constexpr uint8_t FAKE_MODEM = 2;
constexpr uint8_t FAKE_SOCKET = 3;
// Standard routing of the local state: modem and socket draw on the screen, the keyboard
// sends to the modem (looped onto the screen: the local echo) and to the socket.
constexpr uint8_t FAKE_STANDARD_LINKS[4] = {0x4D, 0x42, 0x46, 0x4A};

struct fake_minitel_t {
  bool active;
  bool powered;
  uint32_t baud;
  bool accepts_4800;
  bool wire_echo;  // the wiring sends every byte straight back
  uint8_t links[4];  // routing status of the screen, keyboard, modem and socket
  uint8_t command[5];
  uint8_t command_length;
  uint8_t out[64];
  uint32_t out_baud[64];
  uint8_t out_length;
  uint8_t speed_changes;
};

fake_minitel_t g_minitel;

void fake_minitel_start(uint32_t baud) {
  memset(&g_minitel, 0, sizeof(g_minitel));
  g_minitel.active = true;
  g_minitel.powered = true;
  g_minitel.baud = baud;
  g_minitel.accepts_4800 = true;
  memcpy(g_minitel.links, FAKE_STANDARD_LINKS, sizeof(g_minitel.links));
  rx_clear();
  g_uart_baud = 1200;
  g_uart_echo_filter = true;
  g_uart_capture_len = 0;
}

void fake_minitel_reply(const uint8_t* bytes, uint8_t length) {
  for (uint8_t i = 0; i < length && g_minitel.out_length < sizeof(g_minitel.out); ++i) {
    g_minitel.out[g_minitel.out_length] = bytes[i];
    g_minitel.out_baud[g_minitel.out_length] = g_minitel.baud;
    ++g_minitel.out_length;
  }
}

uint8_t fake_speed_code(uint32_t baud) {
  return baud == 4800 ? 0x76 : 0x64;
}

void fake_minitel_run_command() {
  const uint8_t* command = g_minitel.command;
  if (command[1] == 0x39 && command[2] == 0x74) {  // PRO1 STATUS VITESSE
    const uint8_t reply[] = {0x1B, 0x3A, 0x75, fake_speed_code(g_minitel.baud)};
    fake_minitel_reply(reply, sizeof(reply));
  } else if (command[1] == 0x3A && command[2] == 0x6B) {  // PRO2 PROG
    if (command[3] == 0x76 && g_minitel.accepts_4800) {
      g_minitel.baud = 4800;
      ++g_minitel.speed_changes;
    }
    const uint8_t reply[] = {0x1B, 0x3A, 0x75, fake_speed_code(g_minitel.baud)};
    fake_minitel_reply(reply, sizeof(reply));
  } else if (command[1] == 0x3B && (command[2] == 0x60 || command[2] == 0x61)) {  // aiguillage
    uint8_t& links = g_minitel.links[command[3] - 0x58];
    const uint8_t bit = static_cast<uint8_t>(1U << (command[4] - 0x50));
    links = command[2] == 0x61 ? static_cast<uint8_t>(links | bit) : static_cast<uint8_t>(links & ~bit);
    const uint8_t reply[] = {0x1B, 0x3B, 0x63, command[3], links};
    fake_minitel_reply(reply, sizeof(reply));
  }
}

void fake_minitel_receive(uint8_t byte) {
  if (!g_minitel.active || !g_minitel.powered) {
    return;
  }
  if (g_minitel.wire_echo) {
    rx_push(byte);
  }
  if (g_uart_baud != g_minitel.baud) {
    return;  // noise at its speed
  }
  if (g_minitel.command_length == 0 && byte != 0x1B) {
    return;  // page content
  }
  g_minitel.command[g_minitel.command_length++] = byte;
  if (g_minitel.command_length < 2) {
    return;
  }
  const uint8_t prefix = g_minitel.command[1];
  const uint8_t length = prefix == 0x39 ? 3 : prefix == 0x3A ? 4 : prefix == 0x3B ? 5 : 0;
  if (length == 0) {
    g_minitel.command_length = 0;
  } else if (g_minitel.command_length == length) {
    fake_minitel_run_command();
    g_minitel.command_length = 0;
  }
}

void fake_minitel_deliver() {
  for (uint8_t i = 0; i < g_minitel.out_length; ++i) {
    if (g_minitel.out_baud[i] == g_uart_baud) {
      rx_push(g_minitel.out[i]);
    } else {
      g_rx_error = true;
    }
  }
  g_minitel.out_length = 0;
}

extern "C" void uart_minitel_begin(uint32_t baud, uint32_t, int8_t, int8_t) {
  g_uart_baud = baud;
  rx_clear();
}
extern "C" void uart_minitel_set_baud(uint32_t baud) {
  g_uart_baud = baud;
  rx_clear();
}
extern "C" void uart_minitel_set_echo_filter(bool enabled) {
  g_uart_echo_filter = enabled;
}
extern "C" void uart_minitel_discard_input(void) {
  rx_clear();
}
extern "C" bool uart_minitel_rx_error(void) {
  return g_rx_error;
}
extern "C" void uart_minitel_flush(void) {}
extern "C" void uart_minitel_write(uint8_t c) {
  if (g_uart_capture_len < sizeof(g_uart_capture)) {
    g_uart_capture[g_uart_capture_len++] = c;
  }
  fake_minitel_receive(c);
}
extern "C" bool uart_minitel_available(void) {
  if (g_minitel.active) {
    fake_minitel_deliver();
  }
  return g_rx_head != g_rx_tail;
}
extern "C" uint8_t uart_minitel_read(void) {
  if (g_rx_head == g_rx_tail) {
    return 0;
  }
  const uint8_t byte = g_rx[g_rx_tail];
  g_rx_tail = (g_rx_tail + 1) % sizeof(g_rx);
  return byte;
}

void test_visual_wrap_splits_long_line(void) {
  const char* body = "0123456789012345678901234567890123456789abcde";
  char line[41];

  editor_layout::fill_visual_line(body, 0, 40, line, sizeof(line));
  TEST_ASSERT_EQUAL_STRING_LEN("0123456789012345678901234567890123456789", line, 40);

  editor_layout::fill_visual_line(body, 1, 40, line, sizeof(line));
  TEST_ASSERT_EQUAL_STRING("abcde", line);

  const editor_layout::Position position = editor_layout::position_from_index(body, 40, 40);
  TEST_ASSERT_EQUAL_UINT16(1, position.visual_row);
  TEST_ASSERT_EQUAL_UINT8(0, position.visual_col);
}

void test_index_from_visual_handles_wrapped_rows(void) {
  const char* body = "0123456789012345678901234567890123456789abcde";

  TEST_ASSERT_EQUAL_UINT16(43, editor_layout::index_from_visual(body, 1, 3, 40));
  TEST_ASSERT_EQUAL_UINT16(45, editor_layout::index_from_visual(body, 1, 10, 40));
}

void test_logical_line_helpers_cover_trailing_newline(void) {
  const char* body = "first\nsecond\n";

  TEST_ASSERT_EQUAL_UINT16(3, editor_layout::logical_line_count(body));
  TEST_ASSERT_EQUAL_UINT16(0, editor_layout::index_from_logical_line(body, 0));
  TEST_ASSERT_EQUAL_UINT16(6, editor_layout::index_from_logical_line(body, 1));
  TEST_ASSERT_EQUAL_UINT16(13, editor_layout::index_from_logical_line(body, 2));
}

void test_search_wraps_once(void) {
  const char* body = "abc def abc";
  uint16_t found_index = 0;

  TEST_ASSERT_TRUE(editor_layout::find_forward(body, "abc", 1, &found_index));
  TEST_ASSERT_EQUAL_UINT16(8, found_index);

  TEST_ASSERT_TRUE(editor_layout::find_forward(body, "abc", 9, &found_index));
  TEST_ASSERT_EQUAL_UINT16(0, found_index);

  TEST_ASSERT_FALSE(editor_layout::find_forward(body, "zzz", 0, &found_index));
}

void test_input_parser_accepts_standard_and_extended_space_bytes(void) {
  input_parser_t parser{};
  input_event_t event{};
  input_parser_init(&parser);

  TEST_ASSERT_TRUE(input_parser_feed(&parser, 0x00, &event));
  TEST_ASSERT_EQUAL(INPUT_EVENT_PRINTABLE, event.type);
  TEST_ASSERT_EQUAL_CHAR(' ', event.ch);

  TEST_ASSERT_TRUE(input_parser_feed(&parser, 0x20, &event));
  TEST_ASSERT_EQUAL(INPUT_EVENT_PRINTABLE, event.type);
  TEST_ASSERT_EQUAL_CHAR(' ', event.ch);

  TEST_ASSERT_TRUE(input_parser_feed(&parser, 0xA0, &event));
  TEST_ASSERT_EQUAL(INPUT_EVENT_PRINTABLE, event.type);
  TEST_ASSERT_EQUAL_CHAR(' ', event.ch);
}

void test_api_folder_paths_round_trip_and_hierarchy(void) {
  char local[STORE_MAX_FOLDER_PATH + 1];
  char remote[STORE_MAX_FOLDER_PATH + 1];

  api_document_store_utils::normalize_local_folder_path(" /projects/minios/ ", local, sizeof(local));
  TEST_ASSERT_EQUAL_STRING("/projects/minios", local);

  api_document_store_utils::local_folder_to_remote(local, remote, sizeof(remote));
  TEST_ASSERT_EQUAL_STRING("projects/minios", remote);

  TEST_ASSERT_TRUE(api_document_store_utils::is_immediate_child("/", "/projects"));
  TEST_ASSERT_FALSE(api_document_store_utils::is_immediate_child("/", "/projects/minios"));
  TEST_ASSERT_TRUE(api_document_store_utils::is_immediate_child("/projects", "/projects/minios"));
}

void test_api_document_id_round_trip(void) {
  char id[STORE_MAX_DOCUMENT_ID + 1];
  char folder_path[STORE_MAX_FOLDER_PATH + 1];
  char file_name[STORE_MAX_FILE_NAME_LENGTH + 1];

  TEST_ASSERT_TRUE(api_document_store_utils::build_document_id("/projects/minios",
                                                               "roadmap.md",
                                                               id,
                                                               sizeof(id)));
  TEST_ASSERT_EQUAL_STRING("/projects/minios/roadmap.md", id);

  TEST_ASSERT_TRUE(api_document_store_utils::parse_document_id(id,
                                                               folder_path,
                                                               sizeof(folder_path),
                                                               file_name,
                                                               sizeof(file_name)));
  TEST_ASSERT_EQUAL_STRING("/projects/minios", folder_path);
  TEST_ASSERT_EQUAL_STRING("roadmap.md", file_name);
}

void test_api_parse_document_record_reads_bibliotek_shape(void) {
  DynamicJsonDocument doc(2048);
  const char* json =
      "{\"folder\":\"projects/minios\",\"name\":\"roadmap.md\",\"title\":\"Roadmap\","
      "\"tags\":[\"plan\",\"v1\"],\"content\":\"# Hello\\n\",\"sortOrder\":3,"
      "\"updatedAt\":\"2026-03-24T22:00:00Z\"}";
  TEST_ASSERT_FALSE(deserializeJson(doc, json));

  document_record_t record{};
  TEST_ASSERT_TRUE(api_document_store_utils::parse_document_record(doc.as<JsonVariantConst>(), &record, true));
  TEST_ASSERT_EQUAL_STRING("/projects/minios/roadmap.md", record.id);
  TEST_ASSERT_EQUAL_STRING("/projects/minios", record.folder_path);
  TEST_ASSERT_EQUAL_STRING("Roadmap", record.title);
  TEST_ASSERT_EQUAL_STRING("md", record.extension);
  TEST_ASSERT_EQUAL_STRING("# Hello\n", record.body);
  TEST_ASSERT_EQUAL_UINT16(3, record.version);
  TEST_ASSERT_EQUAL_UINT8(2, record.tag_count);
  TEST_ASSERT_EQUAL_STRING("plan", record.tags[0]);
  TEST_ASSERT_EQUAL_STRING("v1", record.tags[1]);
}

void test_api_parse_document_summaries_filters_folder_and_tag(void) {
  DynamicJsonDocument doc(4096);
  const char* json =
      "["
      "{\"folder\":\"projects/minios\",\"name\":\"roadmap.md\",\"title\":\"Roadmap\",\"tags\":[\"plan\"]},"
      "{\"folder\":\"projects/minios\",\"name\":\"notes.txt\",\"title\":\"Notes\",\"tags\":[\"ops\"]},"
      "{\"folder\":\"notes\",\"name\":\"entry.md\",\"title\":\"Entry\",\"tags\":[\"plan\"]}"
      "]";
  TEST_ASSERT_FALSE(deserializeJson(doc, json));

  document_summary_t summaries[4]{};
  const int count = api_document_store_utils::parse_document_summaries(
      doc.as<JsonVariantConst>(), "/projects/minios", "plan", summaries, 4);

  TEST_ASSERT_EQUAL_INT(1, count);
  TEST_ASSERT_EQUAL_STRING("/projects/minios/roadmap.md", summaries[0].id);
  TEST_ASSERT_EQUAL_STRING("Roadmap", summaries[0].title);
  TEST_ASSERT_EQUAL_STRING("plan", summaries[0].tags_summary);
}

void test_api_parse_folder_items_rebuilds_hierarchy(void) {
  DynamicJsonDocument doc(2048);
  TEST_ASSERT_FALSE(deserializeJson(doc, "[\"notes\",\"projects\",\"projects/minios\",\"projects/ideas\"]"));

  folder_info_t root_items[4]{};
  const int root_count =
      api_document_store_utils::parse_folder_items(doc.as<JsonVariantConst>(), "/", root_items, 4);
  TEST_ASSERT_EQUAL_INT(2, root_count);
  TEST_ASSERT_EQUAL_STRING("/notes", root_items[0].path);
  TEST_ASSERT_EQUAL_STRING("/projects", root_items[1].path);

  folder_info_t project_items[4]{};
  const int project_count =
      api_document_store_utils::parse_folder_items(doc.as<JsonVariantConst>(), "/projects", project_items, 4);
  TEST_ASSERT_EQUAL_INT(2, project_count);
  TEST_ASSERT_EQUAL_STRING("/projects/minios", project_items[0].path);
  TEST_ASSERT_EQUAL_STRING("/projects/ideas", project_items[1].path);
}

void test_api_collect_unique_tags_aggregates_all_files(void) {
  DynamicJsonDocument doc(2048);
  TEST_ASSERT_FALSE(deserializeJson(
      doc,
      "["
      "{\"folder\":\"projects\",\"name\":\"roadmap.md\",\"title\":\"Roadmap\",\"tags\":[\"plan\",\"v1\"]},"
      "{\"folder\":\"projects\",\"name\":\"ops.txt\",\"title\":\"Ops\",\"tags\":[\"ops\",\"plan\"]}"
      "]"));

  char tags[4][STORE_MAX_TAG_LENGTH + 1]{};
  const int count = api_document_store_utils::collect_unique_tags(doc.as<JsonVariantConst>(), tags, 4);
  TEST_ASSERT_EQUAL_INT(3, count);
  TEST_ASSERT_EQUAL_STRING("plan", tags[0]);
  TEST_ASSERT_EQUAL_STRING("v1", tags[1]);
  TEST_ASSERT_EQUAL_STRING("ops", tags[2]);
}

void test_chat_response_parser_reads_openai_content(void) {
  DynamicJsonDocument doc(2048);
  TEST_ASSERT_FALSE(deserializeJson(
      doc,
      "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"Bonjour de Gradient\"}}]}"));

  char reply[CHAT_MAX_REPLY_LENGTH + 1];
  bool truncated = false;
  TEST_ASSERT_TRUE(chat_service_utils::parse_chat_response(
      doc.as<JsonVariantConst>(), reply, sizeof(reply), &truncated));
  TEST_ASSERT_EQUAL_STRING("Bonjour de Gradient", reply);
  TEST_ASSERT_FALSE(truncated);
}

void test_chat_response_parser_handles_content_arrays(void) {
  DynamicJsonDocument doc(4096);
  TEST_ASSERT_FALSE(deserializeJson(
      doc,
      "{\"choices\":[{\"message\":{\"content\":[{\"type\":\"output_text\",\"text\":\"Bonjour\"},"
      "{\"type\":\"output_text\",\"text\":\"\\nMinitel\"}]}}]}"));

  char reply[CHAT_MAX_REPLY_LENGTH + 1];
  bool truncated = false;
  TEST_ASSERT_TRUE(chat_service_utils::parse_chat_response(
      doc.as<JsonVariantConst>(), reply, sizeof(reply), &truncated));
  TEST_ASSERT_EQUAL_STRING("Bonjour\nMinitel", reply);
  TEST_ASSERT_FALSE(truncated);
}

void test_chat_response_parser_rejects_empty_payload(void) {
  DynamicJsonDocument doc(1024);
  TEST_ASSERT_FALSE(deserializeJson(doc, "{\"choices\":[{\"message\":{\"content\":\"   \"}}]}"));

  char reply[CHAT_MAX_REPLY_LENGTH + 1];
  bool truncated = false;
  TEST_ASSERT_FALSE(chat_service_utils::parse_chat_response(
      doc.as<JsonVariantConst>(), reply, sizeof(reply), &truncated));
}

void test_chat_normalize_text_transliterates_utf8_for_minitel(void) {
  const char* input =
      "R\xC3\xA9" "ponse d\xE2\x80\x99" "IA\xE2\x80\xAF" ": c\xC5\x93" "ur, \xC5\x93" "uvre, d\xC3\xA9"
      "j\xC3\xA0\xE2\x80\xA6";
  char output[CHAT_MAX_REPLY_LENGTH + 1];
  bool truncated = false;

  TEST_ASSERT_TRUE(chat_service_utils::normalize_text(input, output, sizeof(output), &truncated));
  TEST_ASSERT_EQUAL_STRING("Reponse d'IA : coeur, oeuvre, deja...", output);
  TEST_ASSERT_FALSE(truncated);
}

void test_chat_history_fifo_keeps_last_four_turns(void) {
  chat_history_t history{};
  chat_service_utils::clear_history(&history);

  TEST_ASSERT_TRUE(chat_service_utils::append_turn(&history, "u1", "r1", nullptr, false));
  TEST_ASSERT_TRUE(chat_service_utils::append_turn(&history, "u2", "r2", nullptr, false));
  TEST_ASSERT_TRUE(chat_service_utils::append_turn(&history, "u3", "r3", nullptr, false));
  TEST_ASSERT_TRUE(chat_service_utils::append_turn(&history, "u4", "r4", nullptr, false));
  TEST_ASSERT_TRUE(chat_service_utils::append_turn(&history, "u5", "r5", "lit notes/a.md", false));

  TEST_ASSERT_EQUAL_UINT8(4, history.count);
  TEST_ASSERT_EQUAL_STRING("u2", history.turns[0].user);
  TEST_ASSERT_EQUAL_STRING("r2", history.turns[0].assistant);
  TEST_ASSERT_EQUAL_STRING("", history.turns[0].actions);
  TEST_ASSERT_EQUAL_STRING("u5", history.turns[3].user);
  TEST_ASSERT_EQUAL_STRING("r5", history.turns[3].assistant);
  TEST_ASSERT_EQUAL_STRING("lit notes/a.md", history.turns[3].actions);
}

void test_chat_history_truncates_long_responses(void) {
  chat_history_t history{};
  chat_service_utils::clear_history(&history);

  char long_reply[CHAT_MAX_REPLY_LENGTH + 80];
  memset(long_reply, 'A', sizeof(long_reply) - 1);
  long_reply[sizeof(long_reply) - 1] = '\0';

  TEST_ASSERT_TRUE(chat_service_utils::append_turn(&history, "prompt", long_reply, nullptr, false));
  TEST_ASSERT_EQUAL_UINT8(1, history.count);
  TEST_ASSERT_TRUE(history.turns[0].assistant_truncated);
  TEST_ASSERT_EQUAL_UINT(CHAT_MAX_REPLY_LENGTH, strlen(history.turns[0].assistant));
}

void test_chat_history_sanitizes_utf8_responses_for_minitel(void) {
  chat_history_t history{};
  chat_service_utils::clear_history(&history);

  const char* assistant =
      "Cr\xC3\xA8" "me br\xC3\xBB" "l\xC3\xA9" "e, gar\xC3\xA7" "on, \xC3\xA0" " bient\xC3\xB4" "t";

  TEST_ASSERT_TRUE(chat_service_utils::append_turn(&history, "prompt", assistant, nullptr, false));
  TEST_ASSERT_EQUAL_UINT8(1, history.count);
  TEST_ASSERT_EQUAL_STRING("Creme brulee, garcon, a bientot", history.turns[0].assistant);
  TEST_ASSERT_FALSE(history.turns[0].assistant_truncated);
}

void test_api_parse_document_record_flags_oversized_body_as_read_only(void) {
  static char body[STORE_MAX_BODY_LENGTH + 64];
  memset(body, 'x', sizeof(body) - 1);
  body[sizeof(body) - 1] = '\0';
  DynamicJsonDocument doc(8192);
  doc["folder"] = "notes";
  doc["name"] = "long.md";
  doc["content"] = static_cast<const char*>(body);

  document_record_t record{};
  TEST_ASSERT_TRUE(api_document_store_utils::parse_document_record(doc.as<JsonVariantConst>(), &record, true));
  TEST_ASSERT_TRUE(record.truncated);

  doc["content"] = "short";
  TEST_ASSERT_TRUE(api_document_store_utils::parse_document_record(doc.as<JsonVariantConst>(), &record, true));
  TEST_ASSERT_FALSE(record.truncated);
}

void test_api_parse_document_record_flags_dropped_tags_as_read_only(void) {
  DynamicJsonDocument doc(2048);
  TEST_ASSERT_FALSE(deserializeJson(
      doc,
      "{\"folder\":\"notes\",\"name\":\"a.md\",\"content\":\"x\","
      "\"tags\":[\"one\",\"two\",\"three\",\"four\",\"five\"]}"));

  document_record_t record{};
  TEST_ASSERT_TRUE(api_document_store_utils::parse_document_record(doc.as<JsonVariantConst>(), &record, true));
  TEST_ASSERT_EQUAL_UINT8(STORE_MAX_TAGS_PER_DOC, record.tag_count);
  TEST_ASSERT_TRUE(record.truncated);
}

void test_api_tags_summary_keeps_every_tag(void) {
  document_record_t record{};
  api_document_store_utils::set_tags_from_csv(
      &record, "infrastructure,networking,raspberry-pi,homelab");
  char summary[STORE_MAX_TAGS_SUMMARY_LENGTH + 1];
  api_document_store_utils::build_tags_summary(record, summary, sizeof(summary));
  TEST_ASSERT_EQUAL_STRING("infrastructure,networking,raspberry-pi,homelab", summary);
}

void test_api_split_long_name_keeps_extension(void) {
  char file_name[STORE_MAX_FILE_NAME_LENGTH + 1];
  char title[STORE_MAX_TITLE_LENGTH + 1];
  char extension[STORE_MAX_EXTENSION_LENGTH + 1];
  api_document_store_utils::split_name_or_filename("compte-rendu-reunion-hebdomadaire.txt",
                                                   file_name,
                                                   sizeof(file_name),
                                                   title,
                                                   sizeof(title),
                                                   extension,
                                                   sizeof(extension));
  TEST_ASSERT_EQUAL_STRING("txt", extension);
  TEST_ASSERT_EQUAL_STRING("compte-rendu-reunion-hebdomadair", title);

  api_document_store_utils::split_name_or_filename(
      " notes/.env ", file_name, sizeof(file_name), title, sizeof(title), extension, sizeof(extension));
  TEST_ASSERT_EQUAL_STRING(".env", title);
  TEST_ASSERT_EQUAL_STRING("md", extension);
}

void test_chat_response_parser_flags_length_finish_as_truncated(void) {
  DynamicJsonDocument doc(2048);
  TEST_ASSERT_FALSE(deserializeJson(
      doc,
      "{\"choices\":[{\"finish_reason\":\"length\",\"message\":{\"content\":\"Debut de reponse\"}}]}"));

  char reply[CHAT_MAX_REPLY_LENGTH + 1];
  bool truncated = false;
  TEST_ASSERT_TRUE(chat_service_utils::parse_chat_response(
      doc.as<JsonVariantConst>(), reply, sizeof(reply), &truncated));
  TEST_ASSERT_TRUE(truncated);
}

void test_timezone_maps_iana_names_to_posix_rules(void) {
  TEST_ASSERT_EQUAL_STRING("CET-1CEST,M3.5.0,M10.5.0/3", timezone_utils::to_posix("Europe/Paris"));
  TEST_ASSERT_EQUAL_STRING("CET-1CEST,M3.5.0,M10.5.0/3", timezone_utils::to_posix("EUROPE/PARIS"));
  TEST_ASSERT_EQUAL_STRING("<+04>-4", timezone_utils::to_posix("Indian/Reunion"));
  TEST_ASSERT_EQUAL_STRING("EST5EDT,M3.2.0,M11.1.0", timezone_utils::to_posix("EST5EDT,M3.2.0,M11.1.0"));
  TEST_ASSERT_EQUAL_STRING("UTC0", timezone_utils::to_posix(""));
}

void test_chat_models_keep_digitalocean_chat_models_only(void) {
  // Excerpt of the DigitalOcean /v1/models listing.
  const char* json =
      "{\"data\":["
      "{\"id\":\"all-mini-lm-l6-v2\",\"owned_by\":\"digitalocean\"},"
      "{\"context_length\":200000,\"id\":\"anthropic-claude-4.5-sonnet\",\"owned_by\":\"anthropic\"},"
      "{\"context_length\":1048576,\"id\":\"deepseek-v4.1-flash\",\"max_output_tokens\":1048576,\"owned_by\":\"digitalocean\"},"
      "{\"created\":1785164400,\"id\":\"kimi-k3\",\"owned_by\":\"digitalocean\"},"
      "{\"context_length\":3600,\"id\":\"nemotron-3-diarization\",\"max_output_tokens\":3600,\"owned_by\":\"digitalocean\"},"
      "{\"context_length\":128000,\"id\":\"openai-gpt-oss-120b\",\"max_output_tokens\":4096,\"owned_by\":\"digitalocean\"},"
      "{\"context_length\":400000,\"id\":\"openai-gpt-5\",\"owned_by\":\"openai\"},"
      "{\"context_length\":32768,\"id\":\"qwen3-tts-voicedesign\",\"max_output_tokens\":1,\"owned_by\":\"digitalocean\"},"
      "{\"id\":\"router:general\",\"owned_by\":\"digitalocean\"},"
      "{\"context_length\":256,\"id\":\"stable-diffusion-3.5-large\",\"max_output_tokens\":1,\"owned_by\":\"digitalocean\"}"
      "],\"object\":\"list\"}";
  DynamicJsonDocument doc(4096);
  TEST_ASSERT_FALSE(deserializeJson(doc, json));

  char models[8][CHAT_MAX_MODEL_ID_LENGTH + 1];
  const int count = chat_service_utils::collect_chat_models(doc.as<JsonVariantConst>(), "digitalocean", models, 8);
  TEST_ASSERT_EQUAL_INT(2, count);
  TEST_ASSERT_EQUAL_STRING("deepseek-v4.1-flash", models[0]);
  TEST_ASSERT_EQUAL_STRING("openai-gpt-oss-120b", models[1]);

  TEST_ASSERT_EQUAL_INT(1, chat_service_utils::collect_chat_models(doc.as<JsonVariantConst>(), "digitalocean", models, 1));
}

void test_ota_compares_dotted_versions(void) {
  TEST_ASSERT_TRUE(ota_utils::compare_versions("1.10.0", "1.9.2") > 0);
  TEST_ASSERT_TRUE(ota_utils::compare_versions("1.1", "1.1.0") == 0);
  TEST_ASSERT_TRUE(ota_utils::compare_versions("1.1.0", "1.1.1") < 0);
  TEST_ASSERT_TRUE(ota_utils::compare_versions("2.0.0-beta", "1.99") > 0);
  TEST_ASSERT_TRUE(ota_utils::compare_versions("", "0.0.1") < 0);
}

void test_ota_parses_manifest(void) {
  ota_utils::ota_manifest_t manifest;
  TEST_ASSERT_TRUE(ota_utils::parse_manifest(
      "{\"version\":\"1.2.0\",\"size\":1063421,\"md5\":\"0123456789ABCDEF0123456789abcdef\"}", &manifest));
  TEST_ASSERT_EQUAL_STRING("1.2.0", manifest.version);
  TEST_ASSERT_EQUAL_UINT32(1063421, manifest.size);
  TEST_ASSERT_EQUAL_STRING("0123456789abcdef0123456789abcdef", manifest.md5);

  TEST_ASSERT_TRUE(ota_utils::parse_manifest("{\"version\":\"1.2.0\",\"md5\":\"nope\"}", &manifest));
  TEST_ASSERT_EQUAL_STRING("", manifest.md5);
  TEST_ASSERT_FALSE(ota_utils::parse_manifest("{\"size\":12}", &manifest));
  TEST_ASSERT_FALSE(ota_utils::parse_manifest("not json", &manifest));

  char host[16];
  ota_utils::url_host("https://docs.example.fr/api", host, sizeof(host));
  TEST_ASSERT_EQUAL_STRING("docs.example.fr", host);
  ota_utils::url_host("http://192.168.1.10:3000", host, sizeof(host));
  TEST_ASSERT_EQUAL_STRING("192.168.1.10:30", host);  // cut to the buffer
  ota_utils::url_host("bibliotek.local", host, sizeof(host));
  TEST_ASSERT_EQUAL_STRING("bibliotek.local", host);
  ota_utils::url_host("", host, sizeof(host));
  TEST_ASSERT_EQUAL_STRING("", host);
}

void test_ota_says_when_the_device_is_already_up_to_date(void) {
  char status[41];
  ota_utils::up_to_date_status("1.2.1", "1.2.1", status, sizeof(status));
  TEST_ASSERT_EQUAL_STRING("Deja a jour (1.2.1)", status);
  ota_utils::up_to_date_status("1.2.1", "1.2.0", status, sizeof(status));
  TEST_ASSERT_EQUAL_STRING("Deja a jour (1.2.1), serveur 1.2.0", status);
  ota_utils::up_to_date_status("1.2.1", "", status, sizeof(status));
  TEST_ASSERT_EQUAL_STRING("Deja a jour (1.2.1), rien de publie", status);

  // A 404 is "nothing published" only when Bibliotek says so.
  TEST_ASSERT_TRUE(ota_utils::nothing_published("{\"error\":\"No firmware published\"}"));
  TEST_ASSERT_FALSE(ota_utils::nothing_published("{\"error\":\"Not found\"}"));
  TEST_ASSERT_FALSE(ota_utils::nothing_published(nullptr));
}

void test_ota_builds_server_urls_like_the_notes_client(void) {
  char url[64];
  TEST_ASSERT_TRUE(ota_utils::server_url("https://docs.example.fr/", "/firmware/manifest", url, sizeof(url)));
  TEST_ASSERT_EQUAL_STRING("https://docs.example.fr/firmware/manifest", url);
  TEST_ASSERT_TRUE(ota_utils::server_url("  https://docs.example.fr// ", "/firmware/manifest", url, sizeof(url)));
  TEST_ASSERT_EQUAL_STRING("https://docs.example.fr/firmware/manifest", url);
  TEST_ASSERT_TRUE(ota_utils::server_url("http://192.168.1.10:3000", "/firmware/download", url, sizeof(url)));
  TEST_ASSERT_EQUAL_STRING("http://192.168.1.10:3000/firmware/download", url);
  TEST_ASSERT_FALSE(ota_utils::server_url("  ", "/firmware/manifest", url, sizeof(url)));
  TEST_ASSERT_FALSE(ota_utils::server_url("https://docs.example.fr", "/firmware/manifest", url, 24));
}

void test_workspace_parses_typed_tasks(void) {
  task_input_t task;
  TEST_ASSERT_TRUE(workspace_utils::parse_task_input("Commander colle !! @25/10 14h30", "2026-10-20", &task));
  TEST_ASSERT_EQUAL_STRING("Commander colle", task.text);
  TEST_ASSERT_EQUAL_STRING("2026-10-25", task.due);
  TEST_ASSERT_EQUAL_STRING("14:30", task.time);
  TEST_ASSERT_EQUAL_UINT8(2, task.priority);

  TEST_ASSERT_TRUE(workspace_utils::parse_task_input("@demain appeler Paul", "2026-12-31", &task));
  TEST_ASSERT_EQUAL_STRING("appeler Paul", task.text);
  TEST_ASSERT_EQUAL_STRING("2027-01-01", task.due);
  TEST_ASSERT_EQUAL_STRING("", task.time);

  // A day already past this year means next year; times only count right after a date.
  TEST_ASSERT_TRUE(workspace_utils::parse_task_input("Bilan @05/01 9h", "2026-10-20", &task));
  TEST_ASSERT_EQUAL_STRING("2027-01-05", task.due);
  TEST_ASSERT_EQUAL_STRING("09:00", task.time);
  TEST_ASSERT_TRUE(workspace_utils::parse_task_input("Rdv 14h30 @2026-11-02", "2026-10-20", &task));
  TEST_ASSERT_EQUAL_STRING("Rdv 14h30", task.text);
  TEST_ASSERT_EQUAL_STRING("2026-11-02", task.due);

  // Invalid dates stay in the text; without a known today, "@25/10" cannot be placed.
  TEST_ASSERT_TRUE(workspace_utils::parse_task_input("Voir @31/02 ok", "2026-10-20", &task));
  TEST_ASSERT_EQUAL_STRING("Voir @31/02 ok", task.text);
  TEST_ASSERT_TRUE(workspace_utils::parse_task_input("Voir @25/10", "", &task));
  TEST_ASSERT_EQUAL_STRING("Voir @25/10", task.text);
  TEST_ASSERT_FALSE(workspace_utils::parse_task_input("!! @auj", "2026-10-20", &task));
}

void test_workspace_dates_and_labels(void) {
  char date[11];
  TEST_ASSERT_TRUE(workspace_utils::add_days("2028-02-28", 1, date, sizeof(date)));
  TEST_ASSERT_EQUAL_STRING("2028-02-29", date);
  TEST_ASSERT_TRUE(workspace_utils::add_days("2026-01-01", -1, date, sizeof(date)));
  TEST_ASSERT_EQUAL_STRING("2025-12-31", date);
  TEST_ASSERT_FALSE(workspace_utils::is_valid_date("2026-13-01"));

  char label[8];
  workspace_utils::format_due_label("2026-10-20", "2026-10-20", label, sizeof(label));
  TEST_ASSERT_EQUAL_STRING("auj.", label);
  workspace_utils::format_due_label("2026-10-21", "2026-10-20", label, sizeof(label));
  TEST_ASSERT_EQUAL_STRING("demain", label);
  workspace_utils::format_due_label("2026-10-02", "2026-10-20", label, sizeof(label));
  TEST_ASSERT_EQUAL_STRING("retard", label);
  workspace_utils::format_due_label("2026-11-03", "2026-10-20", label, sizeof(label));
  TEST_ASSERT_EQUAL_STRING("03/11", label);

  char slug[41];
  workspace_utils::slugify("  Robot Minitel v2 !", slug, sizeof(slug));
  TEST_ASSERT_EQUAL_STRING("robot-minitel-v2", slug);
}

void test_workspace_parses_projects_tasks_and_hits(void) {
  DynamicJsonDocument doc(4096);
  TEST_ASSERT_FALSE(deserializeJson(
      doc,
      "{\"items\":[{\"name\":\"robot\",\"title\":\"Robot \xC3\xA9t\xC3\xA9\",\"status\":\"en cours\","
      "\"next\":\"Souder\",\"openTasks\":3,\"overdueTasks\":1,\"weekMinutes\":90}]}"));
  project_summary_t projects[2];
  TEST_ASSERT_EQUAL_INT(1, workspace_utils::parse_projects(doc.as<JsonVariantConst>(), projects, 2));
  TEST_ASSERT_EQUAL_STRING("robot", projects[0].name);
  TEST_ASSERT_EQUAL_STRING("Robot ete", projects[0].title);
  TEST_ASSERT_EQUAL_UINT16(3, projects[0].open_tasks);
  TEST_ASSERT_EQUAL_UINT16(90, projects[0].week_minutes);

  TEST_ASSERT_FALSE(deserializeJson(
      doc,
      "{\"items\":[{\"file\":\"projets/robot/taches.md\",\"line\":2,\"project\":\"robot\","
      "\"text\":\"Commander\",\"done\":false,\"due\":\"2026-10-19\",\"priority\":7},"
      "{\"file\":\"\",\"line\":3}],\"total\":12}"));
  task_item_t tasks[4];
  int total = 0;
  TEST_ASSERT_EQUAL_INT(1, workspace_utils::parse_tasks(doc.as<JsonVariantConst>(), tasks, 4, &total));
  TEST_ASSERT_EQUAL_INT(12, total);
  TEST_ASSERT_EQUAL_STRING("projets/robot/taches.md", tasks[0].file);
  TEST_ASSERT_EQUAL_UINT16(2, tasks[0].line);
  TEST_ASSERT_EQUAL_STRING("2026-10-19", tasks[0].due);
  TEST_ASSERT_EQUAL_UINT8(3, tasks[0].priority);

  TEST_ASSERT_FALSE(deserializeJson(
      doc, "{\"items\":[{\"id\":\"notes/reunion.md\",\"line\":4,\"snippet\":\"D\xC3\xA9" "cision\"}]}"));
  search_hit_t hits[2];
  TEST_ASSERT_EQUAL_INT(1, workspace_utils::parse_search_hits(doc.as<JsonVariantConst>(), hits, 2));
  TEST_ASSERT_EQUAL_STRING("Decision", hits[0].snippet);
  TEST_ASSERT_EQUAL_UINT16(4, hits[0].line);
}

struct wrapped_lines_t {
  char lines[8][41];
  uint8_t count;
};

void collect_wrapped_line(const char* line, void* context) {
  wrapped_lines_t* out = static_cast<wrapped_lines_t*>(context);
  if (out->count < 8) {
    strncpy(out->lines[out->count], line, sizeof(out->lines[0]) - 1);
    out->lines[out->count][sizeof(out->lines[0]) - 1] = '\0';
    ++out->count;
  }
}

void test_text_wrap_breaks_at_last_space(void) {
  wrapped_lines_t out{};
  TEST_ASSERT_EQUAL_UINT16(
      3, text_wrap::wrap("Q> long question here", "", "", 12, collect_wrapped_line, &out));
  TEST_ASSERT_EQUAL_STRING("Q> long", out.lines[0]);
  TEST_ASSERT_EQUAL_STRING("question", out.lines[1]);
  TEST_ASSERT_EQUAL_STRING("here", out.lines[2]);
}

void test_text_wrap_applies_prefixes_and_newlines(void) {
  wrapped_lines_t out{};
  TEST_ASSERT_EQUAL_UINT16(
      3, text_wrap::wrap("one two three\nfour", "R> ", "   ", 10, collect_wrapped_line, &out));
  TEST_ASSERT_EQUAL_STRING("R> one two", out.lines[0]);
  TEST_ASSERT_EQUAL_STRING("   three", out.lines[1]);
  TEST_ASSERT_EQUAL_STRING("   four", out.lines[2]);
}

void test_text_wrap_cuts_words_longer_than_a_line(void) {
  wrapped_lines_t out{};
  TEST_ASSERT_EQUAL_UINT16(3, text_wrap::wrap("abcdefghij", "", "", 4, collect_wrapped_line, &out));
  TEST_ASSERT_EQUAL_STRING("abcd", out.lines[0]);
  TEST_ASSERT_EQUAL_STRING("efgh", out.lines[1]);
  TEST_ASSERT_EQUAL_STRING("ij", out.lines[2]);
}

void test_text_wrap_drops_spaces_at_soft_breaks(void) {
  wrapped_lines_t out{};
  TEST_ASSERT_EQUAL_UINT16(3, text_wrap::wrap("abc   def \nx", "", "", 4, collect_wrapped_line, &out));
  TEST_ASSERT_EQUAL_STRING("abc", out.lines[0]);
  TEST_ASSERT_EQUAL_STRING("def", out.lines[1]);
  TEST_ASSERT_EQUAL_STRING("x", out.lines[2]);
}

void test_text_wrap_counts_without_sink_and_keeps_empty_text(void) {
  TEST_ASSERT_EQUAL_UINT16(1, text_wrap::wrap("", "Q> ", "   ", 40, nullptr, nullptr));
  TEST_ASSERT_EQUAL_UINT16(2, text_wrap::wrap("Session vide. ENVOI pour poser une question.", "", "", 40, nullptr, nullptr));
}

void test_text_wrap_list_starts_every_item_with_the_prefix(void) {
  wrapped_lines_t out{};
  TEST_ASSERT_EQUAL_UINT16(
      3, text_wrap::wrap_list("lit a.md\nremplace notes/b.md", " * ", "   ", 14, collect_wrapped_line, &out));
  TEST_ASSERT_EQUAL_STRING(" * lit a.md", out.lines[0]);
  TEST_ASSERT_EQUAL_STRING(" * remplace", out.lines[1]);
  TEST_ASSERT_EQUAL_STRING("   notes/b.md", out.lines[2]);
  TEST_ASSERT_EQUAL_UINT16(0, text_wrap::wrap_list("", " * ", "   ", 40, nullptr, nullptr));
}

bool uart_capture_contains(const uint8_t* sequence, size_t len) {
  for (size_t i = 0; i + len <= g_uart_capture_len; ++i) {
    if (memcmp(g_uart_capture + i, sequence, len) == 0) {
      return true;
    }
  }
  return false;
}

void ui_render_row3(const char* text) {
  minitel_ui_render_frame("HOME", "wifi", "12:00", "footer");
  minitel_ui_write_line(3, text);
}

// Full refresh of a frame whose row 3 holds `text`, then clears the capture.
void ui_start_with_row3(const char* text) {
  minitel_ui_begin(1200, 22, 23);
  ui_render_row3(text);
  minitel_ui_flush();
  g_uart_capture_len = 0;
}

// The cursor is already off and each row starts with a US: a diff flush sends the rows only.
void assert_diff_flush(const uint8_t* expected_rows, size_t len) {
  TEST_ASSERT_EQUAL_UINT(len, g_uart_capture_len);
  TEST_ASSERT_EQUAL_MEMORY(expected_rows, g_uart_capture, len);
}

void test_ui_full_refresh_sends_only_visible_content(void) {
  minitel_ui_begin(1200, 22, 23);
  g_uart_capture_len = 0;
  ui_render_row3("Hello");
  minitel_ui_flush();

  const uint8_t clear_page[] = {0x14, 0x0C};
  TEST_ASSERT_EQUAL_MEMORY(clear_page, g_uart_capture, sizeof(clear_page));
  const uint8_t row3[] = {0x1F, 0x43, 0x41, 'H', 'e', 'l', 'l', 'o'};
  TEST_ASSERT_TRUE(uart_capture_contains(row3, sizeof(row3)));
  const uint8_t padded_row3[] = {'H', 'e', 'l', 'l', 'o', ' '};
  TEST_ASSERT_FALSE(uart_capture_contains(padded_row3, sizeof(padded_row3)));
  for (size_t i = 0; i + 1 < g_uart_capture_len; ++i) {
    if (g_uart_capture[i] == 0x1F) {
      TEST_ASSERT_TRUE(g_uart_capture[i + 1] <= 0x40 + UI_ROWS);
    }
  }
  // Blank cells are no longer sent: about 100 bytes instead of ~1000 for this sparse page.
  TEST_ASSERT_LESS_THAN_UINT(160, g_uart_capture_len);
}

void test_ui_diff_erases_stale_tail_with_can(void) {
  ui_start_with_row3("Hello world");
  ui_render_row3("Hello");
  minitel_ui_flush();

  const uint8_t expected[] = {0x1F, 0x43, 0x46, 0x18};
  assert_diff_flush(expected, sizeof(expected));
}

void test_ui_diff_clears_blank_row_with_single_can(void) {
  ui_start_with_row3("Hello");
  ui_render_row3("");
  minitel_ui_flush();

  const uint8_t expected[] = {0x1F, 0x43, 0x41, 0x18};
  assert_diff_flush(expected, sizeof(expected));
}

void test_ui_diff_cans_right_after_last_change(void) {
  ui_start_with_row3("Hello world");
  ui_render_row3("Help");
  minitel_ui_flush();

  const uint8_t expected[] = {0x1F, 0x43, 0x44, 'p', 0x18};
  assert_diff_flush(expected, sizeof(expected));
}

void test_ui_diff_merges_changes_separated_by_short_gaps(void) {
  ui_start_with_row3("abcdef");
  ui_render_row3("aXcdYf");
  minitel_ui_flush();

  const uint8_t expected[] = {0x1F, 0x43, 0x42, 'X', 'c', 'd', 'Y'};
  assert_diff_flush(expected, sizeof(expected));
}

void test_ui_diff_jumps_over_long_unchanged_gaps(void) {
  ui_start_with_row3("abcdefgh");
  ui_render_row3("XbcdefgY");
  minitel_ui_flush();

  const uint8_t expected[] = {0x1F, 0x43, 0x41, 'X', 0x1F, 0x43, 0x48, 'Y'};
  assert_diff_flush(expected, sizeof(expected));
}

void test_ui_flush_sends_nothing_when_unchanged(void) {
  ui_start_with_row3("Hello");
  ui_render_row3("Hello");
  minitel_ui_flush();
  minitel_ui_set_cursor_visible(false);

  TEST_ASSERT_EQUAL_UINT(0, g_uart_capture_len);
}

void test_ui_cursor_is_hidden_while_drawing_then_shown_again(void) {
  ui_start_with_row3("Hello");
  minitel_ui_set_cursor_visible(true);
  minitel_ui_set_cursor_visible(true);
  ui_render_row3("Help");
  minitel_ui_flush();
  minitel_ui_set_cursor_visible(true);

  const uint8_t expected[] = {0x11, 0x14, 0x1F, 0x43, 0x44, 'p', 0x18, 0x11};
  TEST_ASSERT_EQUAL_UINT(sizeof(expected), g_uart_capture_len);
  TEST_ASSERT_EQUAL_MEMORY(expected, g_uart_capture, sizeof(expected));
}

void ui_render_rows_3_and_5(const char* row3, const char* row5) {
  minitel_ui_render_frame("HOME", "wifi", "12:00", "footer");
  minitel_ui_write_line(3, row3);
  minitel_ui_write_line(5, row5);
}

void test_ui_flush_until_input_stops_for_a_waiting_key(void) {
  minitel_ui_begin(1200, 22, 23);
  ui_render_rows_3_and_5("abc", "def");
  minitel_ui_flush();
  g_uart_capture_len = 0;

  ui_render_rows_3_and_5("abX", "deY");
  rx_push('k');
  TEST_ASSERT_FALSE(minitel_ui_flush_until_input());
  const uint8_t row3[] = {0x1F, 0x43, 0x43, 'X'};
  TEST_ASSERT_EQUAL_UINT(sizeof(row3), g_uart_capture_len);
  TEST_ASSERT_EQUAL_MEMORY(row3, g_uart_capture, sizeof(row3));

  // The key handled, the next flush sends what is left.
  uart_minitel_read();
  g_uart_capture_len = 0;
  TEST_ASSERT_TRUE(minitel_ui_flush_until_input());
  const uint8_t row5[] = {0x1F, 0x45, 0x43, 'Y'};
  TEST_ASSERT_EQUAL_UINT(sizeof(row5), g_uart_capture_len);
  TEST_ASSERT_EQUAL_MEMORY(row5, g_uart_capture, sizeof(row5));
}

void test_ui_flush_until_input_never_stops_twice_in_a_row(void) {
  minitel_ui_begin(1200, 22, 23);
  ui_render_rows_3_and_5("abc", "def");
  minitel_ui_flush();

  rx_push('k');  // a held key: input keeps waiting
  ui_render_rows_3_and_5("abX", "deY");
  TEST_ASSERT_FALSE(minitel_ui_flush_until_input());
  ui_render_rows_3_and_5("aXX", "dYY");
  TEST_ASSERT_TRUE(minitel_ui_flush_until_input());
  TEST_ASSERT_FALSE(rows_need_flush(1, UI_ROWS));
  rx_clear();
}

struct byte_capture_t {
  char data[512];
  size_t length;
};

bool capture_bytes(const char* bytes, size_t length, void* context) {
  byte_capture_t* capture = static_cast<byte_capture_t*>(context);
  for (size_t i = 0; i < length && capture->length + 1 < sizeof(capture->data); ++i) {
    capture->data[capture->length++] = bytes[i];
  }
  capture->data[capture->length] = '\0';
  return true;
}

struct event_capture_t {
  char events[4][64];
  uint8_t count;
};

bool capture_event(char* data, size_t, void* context) {
  event_capture_t* capture = static_cast<event_capture_t*>(context);
  if (capture->count < 4) {
    strncpy(capture->events[capture->count], data, sizeof(capture->events[0]) - 1);
    capture->events[capture->count][sizeof(capture->events[0]) - 1] = '\0';
    ++capture->count;
  }
  return true;
}

// Wraps `body` in chunked framing, `chunk_size` bytes per chunk, with an extension on the
// first size line.
size_t to_chunked(const char* body, size_t chunk_size, char* out, size_t out_size) {
  size_t length = 0;
  const size_t body_length = strlen(body);
  for (size_t offset = 0; offset < body_length; offset += chunk_size) {
    const size_t take = body_length - offset < chunk_size ? body_length - offset : chunk_size;
    length += snprintf(out + length, out_size - length, offset == 0 ? "%zx;name=v\r\n" : "%zX\r\n", take);
    memcpy(out + length, body + offset, take);
    length += take;
    length += snprintf(out + length, out_size - length, "\r\n");
  }
  length += snprintf(out + length, out_size - length, "0\r\n\r\n");
  return length;
}

void test_chunked_decoder_reassembles_bytes_split_anywhere(void) {
  const char* body = "5\r\nHello\r\n7;ext=1\r\n, world\r\nA\r\n and more\n\r\n0\r\nTrailer: x\r\n\r\n";

  byte_capture_t whole{};
  chunked_decoder_t decoder;
  chunked_init(&decoder);
  TEST_ASSERT_FALSE(chunked_feed(&decoder, body, strlen(body), capture_bytes, &whole));
  TEST_ASSERT_TRUE(decoder.done);
  TEST_ASSERT_FALSE(decoder.failed);
  TEST_ASSERT_EQUAL_STRING("Hello, world and more\n", whole.data);

  byte_capture_t split{};
  chunked_init(&decoder);
  for (size_t i = 0; body[i] != '\0' && !decoder.done; ++i) {
    chunked_feed(&decoder, body + i, 1, capture_bytes, &split);
  }
  TEST_ASSERT_TRUE(decoder.done);
  TEST_ASSERT_EQUAL_STRING("Hello, world and more\n", split.data);
}

void test_chunked_decoder_flags_malformed_framing(void) {
  byte_capture_t capture{};
  chunked_decoder_t decoder;
  chunked_init(&decoder);
  TEST_ASSERT_FALSE(chunked_feed(&decoder, "zz\r\n", 4, capture_bytes, &capture));
  TEST_ASSERT_TRUE(decoder.failed);

  chunked_init(&decoder);
  TEST_ASSERT_FALSE(chunked_feed(&decoder, "3\r\nabcX", 7, capture_bytes, &capture));
  TEST_ASSERT_TRUE(decoder.failed);
  TEST_ASSERT_FALSE(decoder.done);
}

void test_sse_parser_joins_data_lines_and_skips_the_rest(void) {
  const char* stream =
      ": keep-alive\r\n"
      "event: message\r\n"
      "data: first\r\n"
      "data:second\r\n"
      "id: 7\r\n"
      "\r\n"
      "retry: 100\n"
      "\n"
      "data: [DONE]\r\r"
      "data\n\n";

  event_capture_t capture{};
  sse_parser_t parser;
  sse_init(&parser);
  for (size_t i = 0; stream[i] != '\0'; ++i) {
    TEST_ASSERT_TRUE(sse_feed(&parser, stream + i, 1, capture_event, &capture));
  }
  TEST_ASSERT_EQUAL_UINT8(3, capture.count);
  TEST_ASSERT_EQUAL_STRING("first\nsecond", capture.events[0]);
  TEST_ASSERT_EQUAL_STRING("[DONE]", capture.events[1]);
  TEST_ASSERT_EQUAL_STRING("", capture.events[2]);
}

void test_sse_parser_drops_oversized_events(void) {
  static char stream[SSE_MAX_EVENT_DATA + 64];
  size_t length = 0;
  length += snprintf(stream, sizeof(stream), "data: ");
  memset(stream + length, 'x', SSE_MAX_EVENT_DATA);
  length += SSE_MAX_EVENT_DATA;
  length += snprintf(stream + length, sizeof(stream) - length, "\n\ndata: ok\n\n");

  event_capture_t capture{};
  sse_parser_t parser;
  sse_init(&parser);
  TEST_ASSERT_TRUE(sse_feed(&parser, stream, length, capture_event, &capture));
  TEST_ASSERT_EQUAL_UINT16(1, parser.dropped);
  TEST_ASSERT_EQUAL_UINT8(1, capture.count);
  TEST_ASSERT_EQUAL_STRING("ok", capture.events[0]);
}

void test_chat_text_builder_only_ever_appends(void) {
  char out[64];
  chat_text_builder_t builder;
  chat_service_utils::text_builder_begin(&builder, out, sizeof(out));

  chat_service_utils::text_builder_append(&builder, " \n Bonjour ");
  TEST_ASSERT_EQUAL_STRING("Bonjour", out);  // the space waits for the next word
  chat_service_utils::text_builder_append(&builder, "toi\r");
  TEST_ASSERT_EQUAL_STRING("Bonjour toi", out);
  chat_service_utils::text_builder_append(&builder, "\n\n\n\tCr\xC3");  // CRLF, blank lines
  TEST_ASSERT_EQUAL_STRING("Bonjour toi\n Cr", out);
  chat_service_utils::text_builder_append(&builder, "\xA8me\x01\xE2\x80\xA6  ");
  TEST_ASSERT_EQUAL_STRING("Bonjour toi\n Creme?...", out);
  chat_service_utils::text_builder_finish(&builder);
  TEST_ASSERT_EQUAL_STRING("Bonjour toi\n Creme?...", out);
  TEST_ASSERT_FALSE(builder.truncated);

  // Same cleanup as the whole reply normalized at once.
  char normalized[64];
  bool truncated = false;
  TEST_ASSERT_TRUE(chat_service_utils::normalize_text(
      " \n Bonjour toi\r\n\n\n\tCr\xC3\xA8me\x01\xE2\x80\xA6  ", normalized, sizeof(normalized), &truncated));
  TEST_ASSERT_EQUAL_STRING(normalized, out);
}

void test_chat_text_builder_stops_when_full(void) {
  char out[8];
  chat_text_builder_t builder;
  chat_service_utils::text_builder_begin(&builder, out, sizeof(out));
  chat_service_utils::text_builder_append(&builder, "abc defghij");
  TEST_ASSERT_EQUAL_STRING("abc def", out);
  TEST_ASSERT_TRUE(builder.truncated);
  chat_service_utils::text_builder_append(&builder, "k");
  TEST_ASSERT_EQUAL_STRING("abc def", out);

  chat_service_utils::text_builder_begin(&builder, out, sizeof(out));
  chat_service_utils::text_builder_append(&builder, "ab \xC3");
  chat_service_utils::text_builder_finish(&builder);
  TEST_ASSERT_EQUAL_STRING("ab ?", out);  // a reply cut inside a UTF-8 sequence
}

const char* CHAT_STREAM_SAMPLE =
    ": OPENROUTER PROCESSING\n\n"
    "data: {\"id\":\"c1\",\"object\":\"chat.completion.chunk\",\"choices\":[{\"index\":0,"
    "\"delta\":{\"role\":\"assistant\",\"content\":\"\"},\"finish_reason\":null}]}\n\n"
    "data: {\"id\":\"c1\",\"choices\":[{\"index\":0,\"delta\":{\"content\":null,"
    "\"reasoning_content\":\"L'utilisateur salue.\"},\"finish_reason\":null}]}\n\n"
    "data: {\"id\":\"c1\",\"choices\":[{\"index\":0,\"delta\":{\"content\":\"Bonjour, \"},"
    "\"logprobs\":null,\"finish_reason\":null}],\"usage\":null}\n\n"
    "data: {\"id\":\"c1\",\"choices\":[{\"index\":0,\"delta\":{\"content\":\"\\u00e7a va ?\\n\\n\"},"
    "\"finish_reason\":null}]}\n\n"
    "data: {\"id\":\"c1\",\"choices\":[{\"index\":0,\"delta\":{\"content\":\"Tr\\u00e8s bien.\"},"
    "\"finish_reason\":\"stop\"}]}\n\n"
    "data: {\"id\":\"c1\",\"choices\":[],\"usage\":{\"prompt_tokens\":9}}\n\n"
    "data: [DONE]\n\n";

void test_chat_stream_decodes_openai_chunks(void) {
  char reply[CHAT_MAX_REPLY_LENGTH + 1];
  static chat_stream_t stream;
  chat_service_utils::stream_begin(&stream, reply, sizeof(reply), false);
  const size_t length = strlen(CHAT_STREAM_SAMPLE);
  size_t offset = 0;
  bool more = true;
  while (more && offset < length) {
    const size_t take = length - offset < 7 ? length - offset : 7;
    more = chat_service_utils::stream_feed(&stream, CHAT_STREAM_SAMPLE + offset, take);
    offset += take;
  }
  chat_service_utils::stream_finish(&stream);

  TEST_ASSERT_FALSE(more);
  TEST_ASSERT_TRUE(stream.finished);  // stops at finish_reason, before [DONE]
  TEST_ASSERT_TRUE(stream.reasoning);
  TEST_ASSERT_FALSE(stream.length_limited);
  TEST_ASSERT_FALSE(stream.failed);
  TEST_ASSERT_EQUAL_STRING("Bonjour, ca va ?\nTres bien.", reply);
}

void test_chat_stream_reads_chunked_bodies(void) {
  static char body[2048];
  to_chunked(CHAT_STREAM_SAMPLE, 61, body, sizeof(body));

  char reply[CHAT_MAX_REPLY_LENGTH + 1];
  static chat_stream_t stream;
  chat_service_utils::stream_begin(&stream, reply, sizeof(reply), true);
  for (size_t i = 0; body[i] != '\0'; i += 3) {
    if (!chat_service_utils::stream_feed(&stream, body + i, strnlen(body + i, 3))) {
      break;
    }
  }
  chat_service_utils::stream_finish(&stream);
  TEST_ASSERT_TRUE(stream.finished);
  TEST_ASSERT_FALSE(stream.failed);
  TEST_ASSERT_EQUAL_STRING("Bonjour, ca va ?\nTres bien.", reply);
}

void test_chat_stream_reports_errors_and_limits(void) {
  char reply[CHAT_MAX_REPLY_LENGTH + 1];
  static chat_stream_t stream;

  const char* error = "data: {\"error\":{\"message\":\"overloaded\",\"code\":503}}\n\n";
  chat_service_utils::stream_begin(&stream, reply, sizeof(reply), false);
  TEST_ASSERT_FALSE(chat_service_utils::stream_feed(&stream, error, strlen(error)));
  TEST_ASSERT_TRUE(stream.failed);

  const char* cut =
      "data: {\"choices\":[{\"delta\":{\"content\":\"Debut\"},\"finish_reason\":\"length\"}]}\n\n";
  chat_service_utils::stream_begin(&stream, reply, sizeof(reply), false);
  TEST_ASSERT_FALSE(chat_service_utils::stream_feed(&stream, cut, strlen(cut)));
  TEST_ASSERT_TRUE(stream.length_limited);
  TEST_ASSERT_EQUAL_STRING("Debut", reply);

  // Only reasoning, then the token limit: nothing to show (the service retries).
  const char* thinking =
      "data: {\"choices\":[{\"delta\":{\"reasoning\":\"Hmm\"},\"finish_reason\":null}]}\n\n"
      "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"length\"}]}\n\n";
  chat_service_utils::stream_begin(&stream, reply, sizeof(reply), false);
  chat_service_utils::stream_feed(&stream, thinking, strlen(thinking));
  TEST_ASSERT_TRUE(stream.reasoning);
  TEST_ASSERT_TRUE(stream.length_limited);
  TEST_ASSERT_EQUAL_STRING("", reply);

  // A reply longer than the buffer stops the stream.
  char small[6];
  const char* long_reply = "data: {\"choices\":[{\"delta\":{\"content\":\"Bonjour toi\"}}]}\n\n";
  chat_service_utils::stream_begin(&stream, small, sizeof(small), false);
  TEST_ASSERT_FALSE(chat_service_utils::stream_feed(&stream, long_reply, strlen(long_reply)));
  TEST_ASSERT_TRUE(stream.text.truncated);
  TEST_ASSERT_EQUAL_STRING("Bonjo", small);
}

// Captured from Bibliotek's assistant (server/src/assistant.ts): a question that reads a note,
// then asks before changing it...
const char* ASSISTANT_ASK_SAMPLE =
    "data: {\"choices\":[{\"index\":0,\"delta\":{\"reasoning_content\":\"...\"}}]}\n\n"
    "data: {\"action\":\"lit notes/courses.md\"}\n\n"
    "data: {\"confirm\":{\"id\":\"f1ce8e108e22e96250b8c37e\",\"text\":\"Modifier notes/courses.md : "
    "\\\"- pain\\\" -> \\\"- pain complet\\\"\"}}\n\n"
    "data: [DONE]\n\n";
// ... and the answer to the user's ENVOI, which goes on after it.
const char* ASSISTANT_CONFIRM_SAMPLE =
    "data: {\"action\":\"modifie notes/courses.md\"}\n\n"
    "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"C'est fait : j'ai remplac\xC3\xA9 le pain \"}}]}\n\n"
    "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"par du pain complet.\"}}]}\n\n"
    "data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
    "data: [DONE]\n\n";

bool feed_in_pieces(chat_stream_t* stream, const char* body, size_t piece) {
  const size_t length = strlen(body);
  bool more = true;
  for (size_t offset = 0; more && offset < length; offset += piece) {
    const size_t take = length - offset < piece ? length - offset : piece;
    more = chat_service_utils::stream_feed(stream, body + offset, take);
  }
  return more;
}

void test_chat_stream_pauses_for_the_assistant_confirmation(void) {
  char reply[CHAT_MAX_REPLY_LENGTH + 1] = "";
  char actions[CHAT_MAX_ACTIONS_LENGTH + 1] = "";
  static chat_stream_t stream;
  chat_service_utils::stream_resume(&stream, reply, sizeof(reply), actions, sizeof(actions), false);
  TEST_ASSERT_FALSE(feed_in_pieces(&stream, ASSISTANT_ASK_SAMPLE, 5));
  chat_service_utils::stream_finish(&stream);

  TEST_ASSERT_TRUE(stream.finished);
  TEST_ASSERT_TRUE(stream.reasoning);
  TEST_ASSERT_FALSE(stream.failed);
  TEST_ASSERT_EQUAL_STRING("", reply);
  TEST_ASSERT_EQUAL_STRING("lit notes/courses.md", actions);
  TEST_ASSERT_EQUAL_STRING("f1ce8e108e22e96250b8c37e", stream.confirm.id);
  TEST_ASSERT_EQUAL_STRING("Modifier notes/courses.md : \"- pain\" -> \"- pain complet\"", stream.confirm.text);
}

void test_chat_stream_resumes_the_assistant_reply_after_a_confirmation(void) {
  char reply[CHAT_MAX_REPLY_LENGTH + 1] = "Je regarde.";
  char actions[CHAT_MAX_ACTIONS_LENGTH + 1] = "lit notes/courses.md";
  static char body[2048];
  to_chunked(ASSISTANT_CONFIRM_SAMPLE, 37, body, sizeof(body));

  static chat_stream_t stream;
  chat_service_utils::stream_resume(&stream, reply, sizeof(reply), actions, sizeof(actions), true);
  TEST_ASSERT_EQUAL_UINT(11, stream.text_start);
  TEST_ASSERT_EQUAL_UINT(20, stream.actions_start);
  TEST_ASSERT_FALSE(feed_in_pieces(&stream, body, 7));
  chat_service_utils::stream_finish(&stream);

  TEST_ASSERT_TRUE(stream.finished);
  TEST_ASSERT_FALSE(stream.failed);
  TEST_ASSERT_EQUAL_STRING("", stream.confirm.id);
  TEST_ASSERT_EQUAL_STRING("lit notes/courses.md\nmodifie notes/courses.md", actions);
  TEST_ASSERT_EQUAL_STRING("Je regarde.\nC'est fait : j'ai remplace le pain par du pain complet.", reply);
}

void test_chat_stream_keeps_the_assistant_error_message(void) {
  char reply[CHAT_MAX_REPLY_LENGTH + 1] = "";
  char actions[CHAT_MAX_ACTIONS_LENGTH + 1] = "";
  static chat_stream_t stream;
  const char* body =
      "data: {\"action\":\"lit notes/a.md (echec)\"}\n\n"
      "data: {\"error\":{\"message\":\"IA cl\xC3\xA9 invalide\"}}\n\n"
      "data: [DONE]\n\n";
  chat_service_utils::stream_resume(&stream, reply, sizeof(reply), actions, sizeof(actions), false);
  TEST_ASSERT_FALSE(chat_service_utils::stream_feed(&stream, body, strlen(body)));
  TEST_ASSERT_TRUE(stream.failed);
  TEST_ASSERT_EQUAL_STRING("IA cle invalide", stream.error);
  TEST_ASSERT_EQUAL_STRING("lit notes/a.md (echec)", actions);
}

void test_chat_direct_stream_ignores_assistant_events(void) {
  char reply[CHAT_MAX_REPLY_LENGTH + 1];
  static chat_stream_t stream;
  const char* body =
      "data: {\"action\":\"lit notes/a.md\"}\n\n"
      "data: {\"confirm\":{\"id\":\"abc\",\"text\":\"Supprimer\"}}\n\n"
      "data: {\"choices\":[{\"delta\":{\"content\":\"Bonjour\"},\"finish_reason\":\"stop\"}]}\n\n";
  chat_service_utils::stream_begin(&stream, reply, sizeof(reply), false);
  chat_service_utils::stream_feed(&stream, body, strlen(body));
  TEST_ASSERT_TRUE(stream.finished);
  TEST_ASSERT_EQUAL_STRING("", stream.confirm.id);
  TEST_ASSERT_EQUAL_STRING("Bonjour", reply);
}

void test_chat_actions_end_with_an_ellipsis_once_full(void) {
  char actions[32] = "";
  chat_lines_builder_t lines;
  chat_service_utils::lines_builder_resume(&lines, actions, sizeof(actions));
  chat_service_utils::lines_builder_append(&lines, "lit a.md");
  chat_service_utils::lines_builder_append(&lines, "  cherche\n\"x\" ");
  TEST_ASSERT_EQUAL_STRING("lit a.md\ncherche \"x\"", actions);
  TEST_ASSERT_FALSE(lines.full);
  chat_service_utils::lines_builder_append(&lines, "modifie b.md");
  chat_service_utils::lines_builder_append(&lines, "x");
  TEST_ASSERT_EQUAL_STRING("lit a.md\ncherche \"x\"\n...", actions);
  TEST_ASSERT_TRUE(lines.full);

  // A resumed list knows it is full; one whose last action ends with "..." is not.
  chat_service_utils::lines_builder_resume(&lines, actions, sizeof(actions));
  TEST_ASSERT_TRUE(lines.full);
  strcpy(actions, "lit un-long-nom...");
  chat_service_utils::lines_builder_resume(&lines, actions, sizeof(actions));
  TEST_ASSERT_FALSE(lines.full);
}

void test_chat_agent_request_sends_settings_history_and_question(void) {
  chat_history_t history{};
  chat_service_utils::clear_history(&history);
  TEST_ASSERT_TRUE(chat_service_utils::append_turn(&history, "Range", "C'est fait.", "lit a.md\ncree b.md", false));
  TEST_ASSERT_TRUE(chat_service_utils::append_turn(&history, "Merci", "De rien.", nullptr, false));

  chat_agent_settings_t settings;
  settings.ai_url = "https://inference.do-ai.run/v1";
  settings.ai_key = "sk-1";
  settings.model = "deepseek-v4.1-flash";
  settings.now = "2026-10-20 14:32";
  DynamicJsonDocument doc(2048);
  chat_service_utils::build_agent_request(doc, history, "Et maintenant ?", settings);
  TEST_ASSERT_FALSE(doc.overflowed());
  char json[512];
  serializeJson(doc, json, sizeof(json));
  TEST_ASSERT_EQUAL_STRING(
      "{\"ai\":{\"baseUrl\":\"https://inference.do-ai.run/v1\",\"apiKey\":\"sk-1\","
      "\"model\":\"deepseek-v4.1-flash\"},\"now\":\"2026-10-20 14:32\","
      "\"history\":[{\"user\":\"Range\",\"assistant\":\"C'est fait.\",\"actions\":\"lit a.md\\ncree b.md\"},"
      "{\"user\":\"Merci\",\"assistant\":\"De rien.\"}],\"prompt\":\"Et maintenant ?\"}",
      json);

  // Before NTP the server uses its own clock.
  settings.now = "";
  chat_service_utils::build_agent_request(doc, history, "Et maintenant ?", settings);
  TEST_ASSERT_TRUE(doc["now"].isNull());
}

void test_net_queue_runs_one_request_at_a_time_foreground_first(void) {
  net_queue::queue_t queue;
  net_queue::init(&queue);
  TEST_ASSERT_TRUE(net_queue::idle(&queue));

  const int8_t refresh = net_queue::submit(&queue, 1, net_queue::PRIORITY_BACKGROUND);
  const int8_t health = net_queue::submit(&queue, 2, net_queue::PRIORITY_BACKGROUND);
  const int8_t open = net_queue::submit(&queue, 3, net_queue::PRIORITY_FOREGROUND);
  TEST_ASSERT_NOT_EQUAL(net_queue::NET_QUEUE_NONE, refresh);
  TEST_ASSERT_NOT_EQUAL(net_queue::NET_QUEUE_NONE, health);
  TEST_ASSERT_NOT_EQUAL(net_queue::NET_QUEUE_NONE, open);

  TEST_ASSERT_EQUAL_INT8(open, net_queue::next(&queue));
  net_queue::start(&queue, open);
  TEST_ASSERT_EQUAL_INT8(net_queue::NET_QUEUE_NONE, net_queue::next(&queue));
  TEST_ASSERT_EQUAL_INT8(open, net_queue::foreground(&queue));

  TEST_ASSERT_TRUE(net_queue::finish(&queue, open));
  TEST_ASSERT_EQUAL(net_queue::SLOT_DONE, queue.slots[open].state);
  // While its result is delivered, it no longer blocks a follow-up request.
  TEST_ASSERT_EQUAL_INT8(net_queue::NET_QUEUE_NONE, net_queue::foreground(&queue));
  TEST_ASSERT_EQUAL_INT8(refresh, net_queue::next(&queue));
  net_queue::release(&queue, open);
  TEST_ASSERT_EQUAL(net_queue::SLOT_FREE, queue.slots[open].state);

  net_queue::start(&queue, refresh);
  TEST_ASSERT_FALSE(net_queue::finish(&queue, health));  // not the one in flight
  TEST_ASSERT_TRUE(net_queue::finish(&queue, refresh));
  net_queue::release(&queue, refresh);
  TEST_ASSERT_EQUAL_INT8(health, net_queue::next(&queue));
  net_queue::start(&queue, health);
  TEST_ASSERT_TRUE(net_queue::finish(&queue, health));
  net_queue::release(&queue, health);
  TEST_ASSERT_TRUE(net_queue::idle(&queue));
}

void test_net_queue_keeps_one_foreground_and_merges_background(void) {
  net_queue::queue_t queue;
  net_queue::init(&queue);

  const int8_t first = net_queue::submit(&queue, 3, net_queue::PRIORITY_FOREGROUND);
  TEST_ASSERT_EQUAL_INT8(net_queue::NET_QUEUE_NONE, net_queue::submit(&queue, 4, net_queue::PRIORITY_FOREGROUND));

  const int8_t refresh = net_queue::submit(&queue, 1, net_queue::PRIORITY_BACKGROUND);
  TEST_ASSERT_EQUAL_INT8(refresh, net_queue::submit(&queue, 1, net_queue::PRIORITY_BACKGROUND));
  TEST_ASSERT_TRUE(net_queue::background_pending(&queue, 1));
  TEST_ASSERT_FALSE(net_queue::background_pending(&queue, 2));

  net_queue::start(&queue, net_queue::next(&queue));
  TEST_ASSERT_EQUAL(net_queue::SLOT_RUNNING, queue.slots[first].state);
  TEST_ASSERT_TRUE(net_queue::finish(&queue, first));
  net_queue::release(&queue, first);

  net_queue::start(&queue, net_queue::next(&queue));
  TEST_ASSERT_EQUAL(net_queue::SLOT_RUNNING, queue.slots[refresh].state);
  // A refresh that already runs is not merged with: the new one runs after it.
  const int8_t again = net_queue::submit(&queue, 1, net_queue::PRIORITY_BACKGROUND);
  TEST_ASSERT_NOT_EQUAL(refresh, again);

  // Four slots in all.
  TEST_ASSERT_NOT_EQUAL(net_queue::NET_QUEUE_NONE, net_queue::submit(&queue, 2, net_queue::PRIORITY_BACKGROUND));
  TEST_ASSERT_NOT_EQUAL(net_queue::NET_QUEUE_NONE, net_queue::submit(&queue, 5, net_queue::PRIORITY_FOREGROUND));
  TEST_ASSERT_EQUAL_INT8(net_queue::NET_QUEUE_NONE, net_queue::submit(&queue, 6, net_queue::PRIORITY_BACKGROUND));
}

void test_net_queue_cancels_queued_and_running_foreground(void) {
  net_queue::queue_t queue;
  net_queue::init(&queue);
  TEST_ASSERT_EQUAL(net_queue::CANCEL_NONE, net_queue::cancel_foreground(&queue));

  const int8_t queued = net_queue::submit(&queue, 3, net_queue::PRIORITY_FOREGROUND);
  TEST_ASSERT_EQUAL(net_queue::CANCEL_DROPPED, net_queue::cancel_foreground(&queue));
  TEST_ASSERT_EQUAL(net_queue::SLOT_FREE, queue.slots[queued].state);
  TEST_ASSERT_EQUAL_INT8(net_queue::NET_QUEUE_NONE, net_queue::next(&queue));

  const int8_t running = net_queue::submit(&queue, 3, net_queue::PRIORITY_FOREGROUND);
  net_queue::start(&queue, running);
  TEST_ASSERT_EQUAL(net_queue::CANCEL_STOPPING, net_queue::cancel_foreground(&queue));
  TEST_ASSERT_EQUAL(net_queue::SLOT_CANCELLED, queue.slots[running].state);
  TEST_ASSERT_EQUAL_INT8(net_queue::NET_QUEUE_NONE, net_queue::foreground(&queue));

  // A new request is accepted at once but waits for the network task to let go.
  const int8_t retry = net_queue::submit(&queue, 3, net_queue::PRIORITY_FOREGROUND);
  TEST_ASSERT_NOT_EQUAL(net_queue::NET_QUEUE_NONE, retry);
  TEST_ASSERT_EQUAL_INT8(net_queue::NET_QUEUE_NONE, net_queue::next(&queue));
  TEST_ASSERT_FALSE(net_queue::finish(&queue, running));  // dropped, nothing delivered
  TEST_ASSERT_EQUAL(net_queue::SLOT_FREE, queue.slots[running].state);
  TEST_ASSERT_EQUAL_INT8(retry, net_queue::next(&queue));
}

void test_net_queue_user_request_waits_for_the_boot_connect(void) {
  const uint8_t wifi_connect = 1;
  const uint8_t health = 2;
  const uint8_t wifi_scan = 3;
  net_queue::queue_t queue;
  net_queue::init(&queue);

  // Boot: the Wi-Fi connect runs in the background.
  const int8_t connect = net_queue::submit(&queue, wifi_connect, net_queue::PRIORITY_BACKGROUND);
  TEST_ASSERT_EQUAL_INT8(connect, net_queue::next(&queue));
  net_queue::start(&queue, connect);
  TEST_ASSERT_EQUAL_INT8(net_queue::NET_QUEUE_NONE, net_queue::foreground(&queue));

  // The link comes up meanwhile: the server check waits, asking again merges with it.
  const int8_t check = net_queue::submit(&queue, health, net_queue::PRIORITY_BACKGROUND);
  TEST_ASSERT_EQUAL_INT8(check, net_queue::submit(&queue, health, net_queue::PRIORITY_BACKGROUND));

  // A user request is accepted and shown as pending, but waits for the connect.
  int8_t scan = net_queue::submit(&queue, wifi_scan, net_queue::PRIORITY_FOREGROUND);
  TEST_ASSERT_NOT_EQUAL(net_queue::NET_QUEUE_NONE, scan);
  TEST_ASSERT_EQUAL_INT8(scan, net_queue::foreground(&queue));
  TEST_ASSERT_EQUAL_INT8(net_queue::NET_QUEUE_NONE, net_queue::next(&queue));

  // ANNULATION, or leaving the screen, drops the user's request, never the connect.
  TEST_ASSERT_EQUAL(net_queue::CANCEL_DROPPED, net_queue::cancel_foreground(&queue));
  TEST_ASSERT_EQUAL(net_queue::SLOT_RUNNING, queue.slots[connect].state);
  TEST_ASSERT_EQUAL(net_queue::CANCEL_NONE, net_queue::cancel_foreground(&queue));

  // Asked again, it goes right after the connect, before the older server check.
  scan = net_queue::submit(&queue, wifi_scan, net_queue::PRIORITY_FOREGROUND);
  TEST_ASSERT_TRUE(net_queue::finish(&queue, connect));
  net_queue::release(&queue, connect);
  TEST_ASSERT_EQUAL_INT8(scan, net_queue::next(&queue));
  net_queue::start(&queue, scan);
  TEST_ASSERT_TRUE(net_queue::finish(&queue, scan));
  net_queue::release(&queue, scan);
  TEST_ASSERT_EQUAL_INT8(check, net_queue::next(&queue));
}

void test_wifi_failed_connect_goes_back_to_saved_network(void) {
  using namespace wifi_utils;
  // Another network than the saved one: back to the saved one, failed or cancelled.
  TEST_ASSERT_EQUAL(AFTER_CONNECT_BACK_TO_SAVED, after_failed_connect("Cafe", "Maison", false));
  TEST_ASSERT_EQUAL(AFTER_CONNECT_BACK_TO_SAVED, after_failed_connect("Cafe", "Maison", true));
  // The saved network itself keeps being retried, as at boot.
  TEST_ASSERT_EQUAL(AFTER_CONNECT_STAY, after_failed_connect("Maison", "Maison", false));
  TEST_ASSERT_EQUAL(AFTER_CONNECT_STAY, after_failed_connect("Maison", "Maison", true));
  // Nothing saved: a failure leaves the radio retrying, a cancel stops it.
  TEST_ASSERT_EQUAL(AFTER_CONNECT_STAY, after_failed_connect("Cafe", "", false));
  TEST_ASSERT_EQUAL(AFTER_CONNECT_STOP, after_failed_connect("Cafe", "", true));
  TEST_ASSERT_EQUAL(AFTER_CONNECT_STOP, after_failed_connect("Cafe", nullptr, true));
}

// Restarts the link module as after a boot of the ESP32.
void link_reset_state() {
  g_link = minitel_link_state_t{false, 1200, false, true, 0, 0, 0};
  g_status_pending = false;
  g_status_due_ms = 0;
  g_last_check_ms = 0;
}

bool capture_has_speed_change() {
  const uint8_t prog[] = {0x1B, 0x3A, 0x6B};
  return uart_capture_contains(prog, sizeof(prog));
}

void assert_minitel_routed() {
  TEST_ASSERT_TRUE(g_minitel.links[FAKE_SCREEN] & 0x08);   // the socket draws on the screen
  TEST_ASSERT_FALSE(g_minitel.links[FAKE_SCREEN] & 0x04);  // the modem no longer does
  TEST_ASSERT_TRUE(g_minitel.links[FAKE_SOCKET] & 0x02);   // keys reach the socket
  TEST_ASSERT_FALSE(g_minitel.links[FAKE_SOCKET] & 0x04);
  TEST_ASSERT_FALSE(g_minitel.links[FAKE_MODEM] & 0x02);   // no local echo
  TEST_ASSERT_FALSE(g_minitel.links[FAKE_MODEM] & 0x08);
}

void test_link_reply_reader_picks_replies_out_of_keys(void) {
  reply_reader_t reader = {};
  uint8_t module = 0;
  uint8_t value = 0;
  reply_kind_t kind = REPLY_NONE;
  const uint8_t speed[] = {'a', 0x1B, 0x3A, 0x75, 0x76};
  for (uint8_t byte : speed) {
    kind = read_reply(&reader, byte, &module, &value);
  }
  TEST_ASSERT_EQUAL_INT(REPLY_SPEED, kind);
  TEST_ASSERT_EQUAL_HEX8(0x76, value);

  const uint8_t route[] = {0x13, 0x41, 0x1B, 0x3B, 0x63, 0x58, 0x49};
  for (uint8_t byte : route) {
    kind = read_reply(&reader, byte, &module, &value);
  }
  TEST_ASSERT_EQUAL_INT(REPLY_ROUTE, kind);
  TEST_ASSERT_EQUAL_HEX8(0x58, module);
  TEST_ASSERT_EQUAL_HEX8(0x49, value);
  TEST_ASSERT_FALSE(reader.query_echoed);

  const uint8_t echo[] = {0x1B, 0x39, 0x74};
  for (uint8_t byte : echo) {
    read_reply(&reader, byte, &module, &value);
  }
  TEST_ASSERT_TRUE(reader.query_echoed);
}

void test_link_switches_a_fresh_minitel_to_4800_and_cuts_the_local_echo(void) {
  link_reset_state();
  fake_minitel_start(1200);

  const minitel_link_state_t& link = minitel_link_setup(4800);

  TEST_ASSERT_TRUE(link.online);
  TEST_ASSERT_EQUAL_UINT32(4800, link.baud);
  TEST_ASSERT_EQUAL_UINT32(4800, g_minitel.baud);
  TEST_ASSERT_EQUAL_UINT32(4800, g_uart_baud);
  TEST_ASSERT_TRUE(link.routed);
  TEST_ASSERT_FALSE(link.echo_filter);
  TEST_ASSERT_FALSE(g_uart_echo_filter);
  assert_minitel_routed();
  TEST_ASSERT_EQUAL_HEX8(g_minitel.links[FAKE_SCREEN], link.screen_links);
  TEST_ASSERT_EQUAL_HEX8(g_minitel.links[FAKE_MODEM], link.modem_links);
}

void test_link_finds_a_minitel_left_at_4800(void) {
  link_reset_state();
  fake_minitel_start(4800);  // the ESP32 restarted, the Minitel kept our speed

  const minitel_link_state_t& link = minitel_link_setup(4800);

  TEST_ASSERT_TRUE(link.online);
  TEST_ASSERT_EQUAL_UINT32(4800, link.baud);
  TEST_ASSERT_EQUAL_UINT8(0, g_minitel.speed_changes);
  TEST_ASSERT_FALSE(capture_has_speed_change());
  TEST_ASSERT_TRUE(link.routed);
}

void test_link_stays_at_1200_when_4800_is_refused(void) {
  link_reset_state();
  fake_minitel_start(1200);
  g_minitel.accepts_4800 = false;

  const minitel_link_state_t& link = minitel_link_setup(4800);

  TEST_ASSERT_TRUE(link.online);
  TEST_ASSERT_EQUAL_UINT32(1200, link.baud);
  TEST_ASSERT_EQUAL_UINT32(1200, g_uart_baud);
  TEST_ASSERT_TRUE(link.routed);
  TEST_ASSERT_FALSE(link.echo_filter);
}

void test_link_keeps_the_echo_filter_without_a_minitel(void) {
  link_reset_state();
  fake_minitel_start(1200);
  g_minitel.powered = false;

  const minitel_link_state_t& link = minitel_link_setup(4800);

  TEST_ASSERT_FALSE(link.online);
  TEST_ASSERT_EQUAL_UINT32(1200, link.baud);
  TEST_ASSERT_EQUAL_UINT32(1200, g_uart_baud);
  TEST_ASSERT_TRUE(link.echo_filter);
  TEST_ASSERT_TRUE(g_uart_echo_filter);
}

void test_link_keeps_the_echo_filter_when_the_wiring_echoes(void) {
  link_reset_state();
  fake_minitel_start(1200);
  g_minitel.wire_echo = true;

  const minitel_link_state_t& link = minitel_link_setup(4800);

  TEST_ASSERT_TRUE(link.online);
  TEST_ASSERT_TRUE(link.routed);
  TEST_ASSERT_TRUE(link.echo_filter);  // our bytes still come back
  TEST_ASSERT_TRUE(g_uart_echo_filter);
}

void test_link_routes_again_after_a_connection_attempt(void) {
  link_reset_state();
  fake_minitel_start(1200);
  minitel_link_setup(4800);

  // CONNEXION/FIN sends SEP 0x59, then the Minitel goes back to its standard routing.
  minitel_link_note_status(0x59);
  memcpy(g_minitel.links, FAKE_STANDARD_LINKS, sizeof(g_minitel.links));
  TEST_ASSERT_TRUE(minitel_link_holding_output());
  TEST_ASSERT_EQUAL_INT(MINITEL_LINK_IDLE, minitel_link_poll(4800));  // codes still settling

  g_fake_ms += 600;
  TEST_ASSERT_EQUAL_INT(MINITEL_LINK_CHECKED, minitel_link_poll(4800));
  TEST_ASSERT_FALSE(minitel_link_holding_output());
  TEST_ASSERT_EQUAL_UINT32(4800, minitel_link_state().baud);
  assert_minitel_routed();
}

void test_link_ignores_key_codes(void) {
  link_reset_state();
  minitel_link_note_status(0x41);  // ENVOI
  TEST_ASSERT_FALSE(minitel_link_holding_output());
}

void test_link_keeps_the_page_when_a_connection_attempt_resets_the_speed(void) {
  link_reset_state();
  fake_minitel_start(1200);
  minitel_link_setup(4800);

  minitel_link_note_status(0x59);
  g_minitel.baud = 1200;  // back to the standard speed and routing
  memcpy(g_minitel.links, FAKE_STANDARD_LINKS, sizeof(g_minitel.links));
  g_rx_error = true;  // its next codes come at 1200
  g_fake_ms += 600;

  // Output waited for the check: nothing went out at the wrong speed, no redraw needed.
  TEST_ASSERT_EQUAL_INT(MINITEL_LINK_CHECKED, minitel_link_poll(4800));
  TEST_ASSERT_EQUAL_UINT32(4800, g_minitel.baud);
  TEST_ASSERT_EQUAL_UINT32(4800, minitel_link_state().baud);
  assert_minitel_routed();
}

void test_link_sets_up_again_after_a_minitel_restart(void) {
  link_reset_state();
  fake_minitel_start(1200);
  minitel_link_setup(4800);

  // Switched off and on: back at 1200 with the standard routing, its bytes are garbage at 4800.
  g_minitel.baud = 1200;
  memcpy(g_minitel.links, FAKE_STANDARD_LINKS, sizeof(g_minitel.links));
  g_rx_error = true;
  TEST_ASSERT_EQUAL_INT(MINITEL_LINK_IDLE, minitel_link_poll(4800));  // just checked: wait
  TEST_ASSERT_FALSE(g_rx_error);  // the garbage is dropped meanwhile

  g_rx_error = true;
  g_fake_ms += 2500;
  TEST_ASSERT_EQUAL_INT(MINITEL_LINK_RESTARTED, minitel_link_poll(4800));
  TEST_ASSERT_EQUAL_UINT32(4800, minitel_link_state().baud);
  TEST_ASSERT_EQUAL_UINT32(4800, g_minitel.baud);
  assert_minitel_routed();
}

void test_link_only_asks_the_speed_after_noise(void) {
  link_reset_state();
  fake_minitel_start(1200);
  minitel_link_setup(4800);
  g_fake_ms += 2500;
  g_rx_error = true;  // a glitch: the Minitel still answers at 4800
  g_uart_capture_len = 0;

  TEST_ASSERT_EQUAL_INT(MINITEL_LINK_CHECKED, minitel_link_poll(4800));
  const uint8_t query[] = {0x1B, 0x39, 0x74};
  TEST_ASSERT_EQUAL_UINT(sizeof(query), g_uart_capture_len);
  TEST_ASSERT_EQUAL_MEMORY(query, g_uart_capture, sizeof(query));
}

void test_link_ignores_receive_errors_while_no_minitel_answers(void) {
  link_reset_state();
  fake_minitel_start(1200);
  g_minitel.powered = false;
  minitel_link_setup(4800);
  g_fake_ms += 2500;
  g_rx_error = true;
  g_uart_capture_len = 0;

  TEST_ASSERT_EQUAL_INT(MINITEL_LINK_IDLE, minitel_link_poll(4800));
  TEST_ASSERT_EQUAL_UINT(0, g_uart_capture_len);
  TEST_ASSERT_FALSE(g_rx_error);
}

void test_link_comes_online_with_the_status_codes_of_a_minitel_switched_on(void) {
  link_reset_state();
  fake_minitel_start(1200);
  g_minitel.powered = false;
  minitel_link_setup(4800);

  g_minitel.powered = true;  // switched on, then CONNEXION/FIN
  minitel_link_note_status(0x59);
  g_fake_ms += 600;
  TEST_ASSERT_EQUAL_INT(MINITEL_LINK_RESTARTED, minitel_link_poll(4800));
  TEST_ASSERT_TRUE(minitel_link_state().online);
  TEST_ASSERT_EQUAL_UINT32(4800, minitel_link_state().baud);
  assert_minitel_routed();
}

// Each test starts without the fake Minitel and with nothing waiting on the UART.
void tearDown(void) {
  g_minitel.active = false;
  rx_clear();
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_visual_wrap_splits_long_line);
  RUN_TEST(test_index_from_visual_handles_wrapped_rows);
  RUN_TEST(test_logical_line_helpers_cover_trailing_newline);
  RUN_TEST(test_search_wraps_once);
  RUN_TEST(test_input_parser_accepts_standard_and_extended_space_bytes);
  RUN_TEST(test_api_folder_paths_round_trip_and_hierarchy);
  RUN_TEST(test_api_document_id_round_trip);
  RUN_TEST(test_api_parse_document_record_reads_bibliotek_shape);
  RUN_TEST(test_api_parse_document_summaries_filters_folder_and_tag);
  RUN_TEST(test_api_parse_folder_items_rebuilds_hierarchy);
  RUN_TEST(test_api_collect_unique_tags_aggregates_all_files);
  RUN_TEST(test_chat_response_parser_reads_openai_content);
  RUN_TEST(test_chat_response_parser_handles_content_arrays);
  RUN_TEST(test_chat_response_parser_rejects_empty_payload);
  RUN_TEST(test_chat_normalize_text_transliterates_utf8_for_minitel);
  RUN_TEST(test_chat_history_fifo_keeps_last_four_turns);
  RUN_TEST(test_chat_history_truncates_long_responses);
  RUN_TEST(test_chat_history_sanitizes_utf8_responses_for_minitel);
  RUN_TEST(test_api_parse_document_record_flags_oversized_body_as_read_only);
  RUN_TEST(test_api_parse_document_record_flags_dropped_tags_as_read_only);
  RUN_TEST(test_api_tags_summary_keeps_every_tag);
  RUN_TEST(test_api_split_long_name_keeps_extension);
  RUN_TEST(test_chat_response_parser_flags_length_finish_as_truncated);
  RUN_TEST(test_timezone_maps_iana_names_to_posix_rules);
  RUN_TEST(test_chat_models_keep_digitalocean_chat_models_only);
  RUN_TEST(test_ota_compares_dotted_versions);
  RUN_TEST(test_ota_parses_manifest);
  RUN_TEST(test_ota_says_when_the_device_is_already_up_to_date);
  RUN_TEST(test_ota_builds_server_urls_like_the_notes_client);
  RUN_TEST(test_workspace_parses_typed_tasks);
  RUN_TEST(test_workspace_dates_and_labels);
  RUN_TEST(test_workspace_parses_projects_tasks_and_hits);
  RUN_TEST(test_text_wrap_breaks_at_last_space);
  RUN_TEST(test_text_wrap_applies_prefixes_and_newlines);
  RUN_TEST(test_text_wrap_cuts_words_longer_than_a_line);
  RUN_TEST(test_text_wrap_drops_spaces_at_soft_breaks);
  RUN_TEST(test_text_wrap_counts_without_sink_and_keeps_empty_text);
  RUN_TEST(test_text_wrap_list_starts_every_item_with_the_prefix);
  RUN_TEST(test_ui_full_refresh_sends_only_visible_content);
  RUN_TEST(test_ui_diff_erases_stale_tail_with_can);
  RUN_TEST(test_ui_diff_clears_blank_row_with_single_can);
  RUN_TEST(test_ui_diff_cans_right_after_last_change);
  RUN_TEST(test_ui_diff_merges_changes_separated_by_short_gaps);
  RUN_TEST(test_ui_diff_jumps_over_long_unchanged_gaps);
  RUN_TEST(test_ui_flush_sends_nothing_when_unchanged);
  RUN_TEST(test_ui_cursor_is_hidden_while_drawing_then_shown_again);
  RUN_TEST(test_ui_flush_until_input_stops_for_a_waiting_key);
  RUN_TEST(test_ui_flush_until_input_never_stops_twice_in_a_row);
  RUN_TEST(test_chunked_decoder_reassembles_bytes_split_anywhere);
  RUN_TEST(test_chunked_decoder_flags_malformed_framing);
  RUN_TEST(test_sse_parser_joins_data_lines_and_skips_the_rest);
  RUN_TEST(test_sse_parser_drops_oversized_events);
  RUN_TEST(test_chat_text_builder_only_ever_appends);
  RUN_TEST(test_chat_text_builder_stops_when_full);
  RUN_TEST(test_chat_stream_decodes_openai_chunks);
  RUN_TEST(test_chat_stream_reads_chunked_bodies);
  RUN_TEST(test_chat_stream_reports_errors_and_limits);
  RUN_TEST(test_chat_stream_pauses_for_the_assistant_confirmation);
  RUN_TEST(test_chat_stream_resumes_the_assistant_reply_after_a_confirmation);
  RUN_TEST(test_chat_stream_keeps_the_assistant_error_message);
  RUN_TEST(test_chat_direct_stream_ignores_assistant_events);
  RUN_TEST(test_chat_actions_end_with_an_ellipsis_once_full);
  RUN_TEST(test_chat_agent_request_sends_settings_history_and_question);
  RUN_TEST(test_net_queue_runs_one_request_at_a_time_foreground_first);
  RUN_TEST(test_net_queue_keeps_one_foreground_and_merges_background);
  RUN_TEST(test_net_queue_cancels_queued_and_running_foreground);
  RUN_TEST(test_net_queue_user_request_waits_for_the_boot_connect);
  RUN_TEST(test_wifi_failed_connect_goes_back_to_saved_network);
  RUN_TEST(test_link_reply_reader_picks_replies_out_of_keys);
  RUN_TEST(test_link_switches_a_fresh_minitel_to_4800_and_cuts_the_local_echo);
  RUN_TEST(test_link_finds_a_minitel_left_at_4800);
  RUN_TEST(test_link_stays_at_1200_when_4800_is_refused);
  RUN_TEST(test_link_keeps_the_echo_filter_without_a_minitel);
  RUN_TEST(test_link_keeps_the_echo_filter_when_the_wiring_echoes);
  RUN_TEST(test_link_routes_again_after_a_connection_attempt);
  RUN_TEST(test_link_ignores_key_codes);
  RUN_TEST(test_link_keeps_the_page_when_a_connection_attempt_resets_the_speed);
  RUN_TEST(test_link_sets_up_again_after_a_minitel_restart);
  RUN_TEST(test_link_only_asks_the_speed_after_noise);
  RUN_TEST(test_link_ignores_receive_errors_while_no_minitel_answers);
  RUN_TEST(test_link_comes_online_with_the_status_codes_of_a_minitel_switched_on);
  return UNITY_END();
}
