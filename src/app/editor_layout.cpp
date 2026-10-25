#include "editor_layout.h"

#include <string.h>

namespace editor_layout {
namespace {

uint16_t clamp_index(const char* body, uint16_t index) {
  const uint16_t len = body_length(body);
  return index > len ? len : index;
}

uint16_t line_end(const char* body, uint16_t line_start) {
  const uint16_t len = body_length(body);
  uint16_t index = line_start;
  while (index < len && body[index] != '\n') {
    ++index;
  }
  return index;
}

uint16_t line_visual_rows(uint16_t line_length, uint8_t width) {
  const uint8_t safe_width = width == 0 ? 1 : width;
  return static_cast<uint16_t>(line_length / safe_width + 1);
}

}  // namespace

uint16_t body_length(const char* body) {
  if (!body) {
    return 0;
  }
  return static_cast<uint16_t>(strlen(body));
}

uint16_t visual_row_count(const char* body, uint8_t width) {
  const uint8_t safe_width = width == 0 ? 1 : width;
  const uint16_t len = body_length(body);
  uint16_t total_rows = 0;
  uint16_t line_start = 0;

  while (true) {
    const uint16_t end = line_end(body, line_start);
    total_rows = static_cast<uint16_t>(total_rows + line_visual_rows(end - line_start, safe_width));
    if (end >= len) {
      break;
    }
    line_start = static_cast<uint16_t>(end + 1);
  }

  return total_rows == 0 ? 1 : total_rows;
}

uint16_t logical_line_count(const char* body) {
  const uint16_t len = body_length(body);
  uint16_t lines = 1;
  for (uint16_t i = 0; i < len; ++i) {
    if (body[i] == '\n') {
      ++lines;
    }
  }
  return lines;
}

Position position_from_index(const char* body, uint16_t index, uint8_t width) {
  const uint8_t safe_width = width == 0 ? 1 : width;
  const uint16_t len = clamp_index(body, index);

  Position position{};
  uint16_t line_start = 0;

  for (uint16_t i = 0; i < len; ++i) {
    if (body[i] == '\n') {
      const uint16_t line_length = static_cast<uint16_t>(i - line_start);
      position.visual_row = static_cast<uint16_t>(position.visual_row +
                                                  line_visual_rows(line_length, safe_width));
      ++position.logical_line;
      line_start = static_cast<uint16_t>(i + 1);
    }
  }

  position.logical_col = static_cast<uint16_t>(len - line_start);
  position.visual_row = static_cast<uint16_t>(position.visual_row + (position.logical_col / safe_width));
  position.visual_col = static_cast<uint8_t>(position.logical_col % safe_width);
  return position;
}

uint16_t index_from_visual(const char* body, uint16_t visual_row, uint8_t visual_col, uint8_t width) {
  const uint8_t safe_width = width == 0 ? 1 : width;
  const uint16_t len = body_length(body);
  uint16_t current_row = 0;
  uint16_t line_start = 0;

  while (true) {
    const uint16_t end = line_end(body, line_start);
    const uint16_t line_length = static_cast<uint16_t>(end - line_start);
    const uint16_t rows = line_visual_rows(line_length, safe_width);

    if (visual_row < current_row + rows) {
      const uint16_t row_in_line = static_cast<uint16_t>(visual_row - current_row);
      const uint16_t row_logical_start = static_cast<uint16_t>(row_in_line * safe_width);
      if (row_logical_start >= line_length) {
        return end;
      }

      const uint16_t remaining = static_cast<uint16_t>(line_length - row_logical_start);
      if (remaining < safe_width && visual_col >= remaining) {
        return end;
      }

      return static_cast<uint16_t>(line_start + row_logical_start + visual_col);
    }

    current_row = static_cast<uint16_t>(current_row + rows);
    if (end >= len) {
      return len;
    }
    line_start = static_cast<uint16_t>(end + 1);
  }
}

uint16_t index_from_logical_line(const char* body, uint16_t logical_line) {
  const uint16_t len = body_length(body);
  uint16_t current_line = 0;
  uint16_t index = 0;

  while (index < len && current_line < logical_line) {
    if (body[index++] == '\n') {
      ++current_line;
    }
  }

  return index > len ? len : index;
}

void fill_visual_line(const char* body, uint16_t visual_row, uint8_t width, char* out, size_t out_size) {
  if (!out || out_size == 0) {
    return;
  }

  out[0] = '\0';
  const uint8_t safe_width = width == 0 ? 1 : width;
  const uint16_t len = body_length(body);
  uint16_t current_row = 0;
  uint16_t line_start = 0;

  while (true) {
    const uint16_t end = line_end(body, line_start);
    const uint16_t line_length = static_cast<uint16_t>(end - line_start);
    const uint16_t rows = line_visual_rows(line_length, safe_width);

    if (visual_row < current_row + rows) {
      const uint16_t row_in_line = static_cast<uint16_t>(visual_row - current_row);
      const uint16_t row_logical_start = static_cast<uint16_t>(row_in_line * safe_width);
      if (row_logical_start >= line_length) {
        return;
      }

      size_t out_index = 0;
      for (uint16_t i = static_cast<uint16_t>(line_start + row_logical_start);
           i < end && out_index + 1 < out_size && out_index < safe_width;
           ++i) {
        out[out_index++] = body[i];
      }
      out[out_index] = '\0';
      return;
    }

    current_row = static_cast<uint16_t>(current_row + rows);
    if (end >= len) {
      return;
    }
    line_start = static_cast<uint16_t>(end + 1);
  }
}

bool find_forward(const char* body, const char* query, uint16_t start_index, uint16_t* out_index) {
  if (!body || !query || query[0] == '\0') {
    return false;
  }

  const uint16_t len = body_length(body);
  const uint16_t query_len = body_length(query);
  if (query_len == 0 || query_len > len) {
    return false;
  }

  const uint16_t safe_start = start_index > len ? len : start_index;
  const uint16_t pass_limits[2] = {len, safe_start};
  const uint16_t pass_starts[2] = {safe_start, 0};
  const uint8_t pass_count = safe_start == 0 ? 1 : 2;

  for (uint8_t pass = 0; pass < pass_count; ++pass) {
    for (uint16_t i = pass_starts[pass]; i + query_len <= pass_limits[pass]; ++i) {
      if (strncmp(body + i, query, query_len) == 0) {
        if (out_index) {
          *out_index = i;
        }
        return true;
      }
    }
  }

  return false;
}

}  // namespace editor_layout
