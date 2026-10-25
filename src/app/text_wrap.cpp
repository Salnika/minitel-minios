#include "text_wrap.h"

#include <string.h>

namespace text_wrap {

uint16_t wrap(const char* text,
              const char* prefix,
              const char* continuation_prefix,
              uint8_t width,
              line_sink_t sink,
              void* context) {
  if (width == 0) {
    return 0;
  }
  if (width > TEXT_WRAP_MAX_WIDTH) {
    width = TEXT_WRAP_MAX_WIDTH;
  }

  const char* source = text ? text : "";
  const char* current_prefix = prefix ? prefix : "";
  uint16_t line_count = 0;

  while (true) {
    char line[TEXT_WRAP_MAX_WIDTH + 1];
    size_t prefix_len = strlen(current_prefix);
    if (prefix_len >= width) {
      prefix_len = width - 1;  // keep room for at least one character of text
    }
    memcpy(line, current_prefix, prefix_len);
    const size_t room = width - prefix_len;

    size_t segment_len = 0;
    while (source[segment_len] != '\0' && source[segment_len] != '\n') {
      ++segment_len;
    }

    size_t take = segment_len;
    bool soft_break = false;
    if (segment_len > room) {
      size_t break_at = room;
      while (break_at > 0 && source[break_at] != ' ') {
        --break_at;
      }
      take = break_at > 0 ? break_at : room;
      soft_break = break_at > 0;
    }

    size_t shown = take;
    while (shown > 0 && source[shown - 1] == ' ') {
      --shown;  // trailing spaces are invisible, drop them
    }
    memcpy(line + prefix_len, source, shown);
    line[prefix_len + shown] = '\0';
    if (sink) {
      sink(line, context);
    }
    ++line_count;

    source += take;
    if (soft_break) {
      while (*source == ' ') {
        ++source;
      }
      if (*source == '\0') {
        break;
      }
      if (*source == '\n') {
        ++source;  // the wrapped line already ends here
      }
    } else if (take == segment_len) {
      if (*source == '\0') {
        break;
      }
      ++source;  // consume '\n'
    }
    current_prefix = continuation_prefix ? continuation_prefix : "";
  }

  return line_count;
}

uint16_t wrap_list(const char* text,
                   const char* prefix,
                   const char* continuation_prefix,
                   uint8_t width,
                   line_sink_t sink,
                   void* context) {
  uint16_t line_count = 0;
  const char* source = text ? text : "";
  while (*source != '\0') {
    const char* end = strchr(source, '\n');
    const size_t length = end ? static_cast<size_t>(end - source) : strlen(source);
    char item[TEXT_WRAP_MAX_ITEM + 1];
    const size_t take = length < TEXT_WRAP_MAX_ITEM ? length : TEXT_WRAP_MAX_ITEM;
    memcpy(item, source, take);
    item[take] = '\0';
    line_count = static_cast<uint16_t>(line_count + wrap(item, prefix, continuation_prefix, width, sink, context));
    if (!end) {
      break;
    }
    source = end + 1;
  }
  return line_count;
}

}  // namespace text_wrap
