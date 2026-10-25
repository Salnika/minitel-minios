#include "input.h"

#include <ctype.h>

#include "../platform/arduino_wrapper.h"

namespace {
constexpr uint8_t SEP = 0x13;
constexpr uint8_t SS2 = 0x19;
constexpr uint32_t SPECIAL_REPEAT_WINDOW_MS = 140;

char normalize_printable_byte(uint8_t byte) {
  switch (byte) {
    case 0x00:
    case 0xA0:
      return ' ';
    default:
      return static_cast<char>(byte);
  }
}
}

void input_parser_init(input_parser_t* parser) {
  if (!parser) {
    return;
  }
  parser->sequence_state = 0;
  parser->last_special_code = 0;
  parser->last_special_ms = 0;
}

static bool make_event(input_event_t* event, input_event_type_t type, char ch = '\0') {
  if (!event) {
    return false;
  }
  event->type = type;
  event->ch = ch;
  return true;
}

static bool decode_special_code(uint8_t byte, input_event_t* event) {
  switch (byte) {
    case 'A': return make_event(event, INPUT_EVENT_ENTER);
    case 'B': return make_event(event, INPUT_EVENT_ESCAPE);
    case 'C': return make_event(event, INPUT_EVENT_UP);
    case 'D': return make_event(event, INPUT_EVENT_RIGHT);
    case 'E': return make_event(event, INPUT_EVENT_CANCEL);
    case 'F': return make_event(event, INPUT_EVENT_LEFT);
    case 'G': return make_event(event, INPUT_EVENT_BACKSPACE);
    case 'H': return make_event(event, INPUT_EVENT_DOWN);
    case 'I':
    case 'Y':
      return make_event(event, INPUT_EVENT_SAVE);
    default:
      return false;
  }
}

static bool make_special_event(input_parser_t* parser, uint8_t byte, input_event_t* event) {
  if (!decode_special_code(byte, event)) {
    return false;
  }
  if (parser) {
    parser->last_special_code = byte;
    parser->last_special_ms = get_millis();
  }
  return true;
}

static bool maybe_repeat_special(input_parser_t* parser, uint8_t byte, input_event_t* event) {
  if (!parser || parser->last_special_code != byte) {
    return false;
  }

  const uint32_t now = get_millis();
  if (now - parser->last_special_ms > SPECIAL_REPEAT_WINDOW_MS) {
    return false;
  }

  return make_special_event(parser, byte, event);
}

bool input_parser_feed(input_parser_t* parser, uint8_t byte, input_event_t* event) {
  if (!parser || !event) {
    return false;
  }

  event->type = INPUT_EVENT_NONE;
  event->ch = '\0';

  if (parser->sequence_state == 1) {
    parser->sequence_state = 0;
    return make_special_event(parser, byte, event);
  }

  if (parser->sequence_state == 2) {
    parser->sequence_state = 0;
    switch (byte) {
      case 'A': return make_event(event, INPUT_EVENT_PRINTABLE, '`');
      case 'B': return make_event(event, INPUT_EVENT_PRINTABLE, '\'');
      case 'C': return make_event(event, INPUT_EVENT_PRINTABLE, '^');
      case 'H': return make_event(event, INPUT_EVENT_PRINTABLE, '"');
      default:
        return false;
    }
  }

  if (byte == SEP) {
    parser->sequence_state = 1;
    return false;
  }

  if (byte == SS2) {
    parser->sequence_state = 2;
    return false;
  }

  if (maybe_repeat_special(parser, byte, event)) {
    return true;
  }

  switch (byte) {
    case 0x08: return make_event(event, INPUT_EVENT_LEFT);
    case 0x09: return make_event(event, INPUT_EVENT_RIGHT);
    case 0x0A: return make_event(event, INPUT_EVENT_DOWN);
    case 0x0B: return make_event(event, INPUT_EVENT_UP);
    case 0x0D: return make_event(event, INPUT_EVENT_ENTER);
    case 0x1E: return make_event(event, INPUT_EVENT_HOME);
    default:
      break;
  }

  if ((byte >= 0x20 && byte <= 0x7E) || byte == 0x00 || byte == 0xA0) {
    return make_event(event, INPUT_EVENT_PRINTABLE, normalize_printable_byte(byte));
  }

  return false;
}
