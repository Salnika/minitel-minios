#ifndef SERVICES_SSE_PARSER_H
#define SERVICES_SSE_PARSER_H

#include <stddef.h>
#include <stdint.h>

// Incremental decoders for a streamed HTTP body: the chunked transfer framing, then the
// server-sent events (text/event-stream) it carries. Bytes may arrive split anywhere.

constexpr size_t SSE_MAX_EVENT_DATA = 1024;
constexpr size_t SSE_MAX_FIELD_NAME = 8;

// Receives decoded body bytes; returns false to stop decoding.
typedef bool (*chunk_data_sink_t)(const char* bytes, size_t length, void* context);

struct chunked_decoder_t {
  uint8_t state;
  uint32_t remaining;  // data bytes left in the current chunk
  uint8_t size_digits;
  bool done;    // the last (empty) chunk was read
  bool failed;  // malformed framing
};

void chunked_init(chunked_decoder_t* decoder);
// Decodes raw bytes and hands the chunk data to `sink`. Returns false once decoding is
// over: the sink stopped, the last chunk was read (`done`) or the framing is malformed
// (`failed`).
bool chunked_feed(chunked_decoder_t* decoder,
                  const char* bytes,
                  size_t length,
                  chunk_data_sink_t sink,
                  void* context);

// Receives the data of each event (data lines joined by '\n', NUL-terminated); the sink may
// modify it in place. Returns false to stop parsing.
typedef bool (*sse_event_sink_t)(char* data, size_t length, void* context);

struct sse_parser_t {
  uint8_t state;
  bool after_cr;  // the previous byte ended a line with CR: a LF right after it is the same end
  char field[SSE_MAX_FIELD_NAME + 1];
  uint8_t field_length;
  bool field_too_long;
  char data[SSE_MAX_EVENT_DATA + 1];
  size_t data_length;
  bool has_data;       // the current event has at least one data line
  bool overflow;       // its data did not fit: the event is dropped
  uint16_t dropped;    // events dropped so far
};

void sse_init(sse_parser_t* parser);
// Parses bytes of an event stream; comments, "event", "id" and "retry" fields are skipped.
// Returns false when the sink asked to stop.
bool sse_feed(sse_parser_t* parser,
              const char* bytes,
              size_t length,
              sse_event_sink_t sink,
              void* context);

#endif
