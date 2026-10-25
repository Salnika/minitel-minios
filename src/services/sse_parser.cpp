#include "sse_parser.h"

#include <string.h>

namespace {

enum chunked_state_t : uint8_t {
  CHUNK_SIZE = 0,
  CHUNK_EXTENSION,  // ";name=value" or padding up to the end of the size line
  CHUNK_SIZE_LF,
  CHUNK_DATA,
  CHUNK_DATA_CR,  // CRLF closing the chunk data
  CHUNK_DATA_LF,
};

enum sse_state_t : uint8_t {
  SSE_LINE_START = 0,
  SSE_FIELD,
  SSE_VALUE_START,  // right after ':', one leading space is dropped
  SSE_VALUE,
  SSE_COMMENT,
};

int hex_value(char ch) {
  if (ch >= '0' && ch <= '9') {
    return ch - '0';
  }
  if (ch >= 'a' && ch <= 'f') {
    return ch - 'a' + 10;
  }
  if (ch >= 'A' && ch <= 'F') {
    return ch - 'A' + 10;
  }
  return -1;
}

void start_chunk_size(chunked_decoder_t* decoder) {
  decoder->state = CHUNK_SIZE;
  decoder->remaining = 0;
  decoder->size_digits = 0;
}

void end_chunk_size_line(chunked_decoder_t* decoder) {
  if (decoder->remaining == 0) {
    decoder->done = true;  // last chunk: the trailers do not matter here
  } else {
    decoder->state = CHUNK_DATA;
  }
}

bool is_data_field(const sse_parser_t* parser) {
  return !parser->field_too_long && parser->field_length == 4 && memcmp(parser->field, "data", 4) == 0;
}

void append_field_char(sse_parser_t* parser, char ch) {
  if (parser->field_length < SSE_MAX_FIELD_NAME) {
    parser->field[parser->field_length++] = ch;
    parser->field[parser->field_length] = '\0';
  } else {
    parser->field_too_long = true;
  }
}

void append_data_char(sse_parser_t* parser, char ch) {
  if (parser->data_length < SSE_MAX_EVENT_DATA) {
    parser->data[parser->data_length++] = ch;
  } else {
    parser->overflow = true;
  }
}

void start_field_value(sse_parser_t* parser) {
  if (is_data_field(parser)) {
    parser->has_data = true;
  }
}

// End of a field line: each data line adds a '\n' to the event data.
void end_field_line(sse_parser_t* parser) {
  if (is_data_field(parser)) {
    parser->has_data = true;
    append_data_char(parser, '\n');
  }
  parser->state = SSE_LINE_START;
}

void reset_event(sse_parser_t* parser) {
  parser->data_length = 0;
  parser->data[0] = '\0';
  parser->has_data = false;
  parser->overflow = false;
}

// Blank line: hands the event over. Returns false when the sink asked to stop.
bool dispatch_event(sse_parser_t* parser, sse_event_sink_t sink, void* context) {
  if (!parser->has_data) {
    reset_event(parser);
    return true;
  }
  if (parser->overflow) {
    ++parser->dropped;
    reset_event(parser);
    return true;
  }
  if (parser->data_length > 0 && parser->data[parser->data_length - 1] == '\n') {
    --parser->data_length;
  }
  parser->data[parser->data_length] = '\0';
  const bool keep_going = sink ? sink(parser->data, parser->data_length, context) : true;
  reset_event(parser);
  return keep_going;
}

}  // namespace

void chunked_init(chunked_decoder_t* decoder) {
  if (!decoder) {
    return;
  }
  memset(decoder, 0, sizeof(*decoder));
  start_chunk_size(decoder);
}

bool chunked_feed(chunked_decoder_t* decoder,
                  const char* bytes,
                  size_t length,
                  chunk_data_sink_t sink,
                  void* context) {
  if (!decoder || decoder->done || decoder->failed) {
    return false;
  }

  size_t i = 0;
  while (i < length) {
    const char ch = bytes[i];
    switch (decoder->state) {
      case CHUNK_SIZE: {
        const int digit = hex_value(ch);
        if (digit >= 0) {
          if (decoder->remaining > 0x0FFFFFFFu) {
            decoder->failed = true;  // would overflow: not a sane chunk size
            return false;
          }
          decoder->remaining = decoder->remaining * 16u + static_cast<uint32_t>(digit);
          ++decoder->size_digits;
          ++i;
          break;
        }
        if (decoder->size_digits == 0) {
          decoder->failed = true;
          return false;
        }
        if (ch == '\r') {
          decoder->state = CHUNK_SIZE_LF;
        } else if (ch == '\n') {
          end_chunk_size_line(decoder);
        } else {
          decoder->state = CHUNK_EXTENSION;
        }
        ++i;
        break;
      }

      case CHUNK_EXTENSION:
        if (ch == '\r') {
          decoder->state = CHUNK_SIZE_LF;
        } else if (ch == '\n') {
          end_chunk_size_line(decoder);
        }
        ++i;
        break;

      case CHUNK_SIZE_LF:
        if (ch != '\n') {
          decoder->failed = true;
          return false;
        }
        end_chunk_size_line(decoder);
        ++i;
        break;

      case CHUNK_DATA: {
        size_t take = length - i;
        if (take > decoder->remaining) {
          take = decoder->remaining;
        }
        decoder->remaining -= static_cast<uint32_t>(take);
        if (decoder->remaining == 0) {
          decoder->state = CHUNK_DATA_CR;
        }
        const bool keep_going = sink ? sink(bytes + i, take, context) : true;
        i += take;
        if (!keep_going) {
          return false;
        }
        break;
      }

      case CHUNK_DATA_CR:
        if (ch == '\r') {
          decoder->state = CHUNK_DATA_LF;
        } else if (ch == '\n') {
          start_chunk_size(decoder);  // bare LF: tolerated
        } else {
          decoder->failed = true;
          return false;
        }
        ++i;
        break;

      case CHUNK_DATA_LF:
        if (ch != '\n') {
          decoder->failed = true;
          return false;
        }
        start_chunk_size(decoder);
        ++i;
        break;

      default:
        decoder->failed = true;
        return false;
    }

    if (decoder->done) {
      return false;
    }
  }
  return true;
}

void sse_init(sse_parser_t* parser) {
  if (!parser) {
    return;
  }
  memset(parser, 0, sizeof(*parser));
  parser->state = SSE_LINE_START;
}

bool sse_feed(sse_parser_t* parser,
              const char* bytes,
              size_t length,
              sse_event_sink_t sink,
              void* context) {
  if (!parser || !bytes) {
    return false;
  }

  for (size_t i = 0; i < length; ++i) {
    const char ch = bytes[i];
    if (ch == '\n' && parser->after_cr) {
      parser->after_cr = false;  // second half of CRLF
      continue;
    }
    parser->after_cr = ch == '\r';
    const bool end_of_line = ch == '\r' || ch == '\n';

    switch (parser->state) {
      case SSE_LINE_START:
        if (end_of_line) {
          if (!dispatch_event(parser, sink, context)) {
            return false;
          }
        } else if (ch == ':') {
          parser->state = SSE_COMMENT;
        } else {
          parser->field_length = 0;
          parser->field[0] = '\0';
          parser->field_too_long = false;
          append_field_char(parser, ch);
          parser->state = SSE_FIELD;
        }
        break;

      case SSE_FIELD:
        if (end_of_line) {
          end_field_line(parser);  // a field name alone has an empty value
        } else if (ch == ':') {
          start_field_value(parser);
          parser->state = SSE_VALUE_START;
        } else {
          append_field_char(parser, ch);
        }
        break;

      case SSE_VALUE_START:
      case SSE_VALUE:
        if (end_of_line) {
          end_field_line(parser);
        } else if (parser->state == SSE_VALUE_START && ch == ' ') {
          parser->state = SSE_VALUE;
        } else {
          parser->state = SSE_VALUE;
          if (is_data_field(parser)) {
            append_data_char(parser, ch);
          }
        }
        break;

      case SSE_COMMENT:
        if (end_of_line) {
          parser->state = SSE_LINE_START;
        }
        break;

      default:
        parser->state = SSE_LINE_START;
        break;
    }
  }
  return true;
}
