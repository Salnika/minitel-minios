#ifndef APP_TEXT_WRAP_H
#define APP_TEXT_WRAP_H

#include <stddef.h>
#include <stdint.h>

namespace text_wrap {

constexpr uint8_t TEXT_WRAP_MAX_WIDTH = 80;
// Longest item of wrap_list(): longer ones are cut.
constexpr uint8_t TEXT_WRAP_MAX_ITEM = 160;

// Receives each wrapped line (NUL-terminated, at most `width` characters).
typedef void (*line_sink_t)(const char* line, void* context);

// Wraps `text` into lines of at most `width` columns: the first line starts with `prefix`,
// the next ones with `continuation_prefix`. Lines break on '\n' and, when too long, at the
// last space that fits (a word longer than a line is cut). `sink` may be null to only
// count lines. Returns the number of lines produced.
uint16_t wrap(const char* text,
              const char* prefix,
              const char* continuation_prefix,
              uint8_t width,
              line_sink_t sink,
              void* context);

// Wraps each line of `text` as a list item: its first row starts with `prefix`, the next
// ones with `continuation_prefix`. An empty text gives no line.
uint16_t wrap_list(const char* text,
                   const char* prefix,
                   const char* continuation_prefix,
                   uint8_t width,
                   line_sink_t sink,
                   void* context);

}  // namespace text_wrap

#endif
