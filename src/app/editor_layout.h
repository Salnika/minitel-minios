#ifndef APP_EDITOR_LAYOUT_H
#define APP_EDITOR_LAYOUT_H

#include <stddef.h>
#include <stdint.h>

namespace editor_layout {

struct Position {
  uint16_t visual_row;
  uint8_t visual_col;
  uint16_t logical_line;
  uint16_t logical_col;
};

uint16_t body_length(const char* body);
uint16_t visual_row_count(const char* body, uint8_t width);
uint16_t logical_line_count(const char* body);
Position position_from_index(const char* body, uint16_t index, uint8_t width);
uint16_t index_from_visual(const char* body, uint16_t visual_row, uint8_t visual_col, uint8_t width);
uint16_t index_from_logical_line(const char* body, uint16_t logical_line);
void fill_visual_line(const char* body, uint16_t visual_row, uint8_t width, char* out, size_t out_size);
bool find_forward(const char* body, const char* query, uint16_t start_index, uint16_t* out_index);

}  // namespace editor_layout

#endif
