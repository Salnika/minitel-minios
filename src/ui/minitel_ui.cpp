#include "minitel_ui.h"

#include <Arduino.h>
#include <stdio.h>
#include <string.h>

#include "lib_minitel/gfx/graphics_engine.h"
#include "lib_minitel/gfx/mosaic_map.h"
#include "lib_minitel/hal/bind.h"
#include "platform/arduino_wrapper.h"

namespace {
constexpr uint32_t MT_DELAY_CLEAR_MS = 40;
constexpr uint8_t MT_BELL = 0x07;
constexpr uint8_t MT_CAN = 0x18;  // fills from the cursor to the end of the row with spaces
constexpr bool UI_GRAPHICS_CONTIGUOUS = true;
// A US cursor jump costs 3 bytes: rewriting up to 2 unchanged cells is cheaper.
constexpr uint8_t UI_SEGMENT_MERGE_GAP = 2;

char g_screen_buffer[UI_ROWS][UI_COLS + 1];
char g_sent_buffer[UI_ROWS][UI_COLS + 1];
bool g_has_sent_buffer = false;
bool g_force_full_refresh = true;

enum cursor_state_t : uint8_t { CURSOR_UNKNOWN, CURSOR_HIDDEN, CURSOR_SHOWN };
cursor_state_t g_cursor_state = CURSOR_UNKNOWN;
// The last flush stopped for a waiting key: the next one goes to the end, so the bottom
// rows still show while a key is held down.
bool g_last_flush_interrupted = false;

struct row_mosaic_overlay_t {
  bool active;
  uint8_t start_col;
  uint8_t width;
  uint8_t masks[UI_COLS];
};

row_mosaic_overlay_t g_screen_mosaic_overlays[UI_ROWS];
row_mosaic_overlay_t g_sent_mosaic_overlays[UI_ROWS];

uint8_t sanitize_char(uint8_t ch) {
  return (ch >= 0x20 && ch <= 0x7E) ? ch : static_cast<uint8_t>('.');
}

void write_bytes(const uint8_t* data, size_t len) {
  for (size_t i = 0; i < len; ++i) {
    uart_minitel_write(data[i]);
  }
}

void print_text(const char* text) {
  if (!text) {
    return;
  }
  while (*text) {
    uart_minitel_write(sanitize_char(static_cast<uint8_t>(*text++)));
  }
}

void clear_screen_impl(void) {
  uart_minitel_write(0x0C);
  uart_minitel_flush();
  delay_ms(MT_DELAY_CLEAR_MS);
}

void set_cursor_impl(uint8_t row, uint8_t col) {
  if (row < 1) {
    row = 1;
  }
  if (row > UI_ROWS) {
    row = UI_ROWS;
  }
  if (col < 1) {
    col = 1;
  }
  if (col > UI_COLS) {
    col = UI_COLS;
  }

  uint8_t cursor_seq[3] = {
    0x1F,
    static_cast<uint8_t>(0x40 + row),
    static_cast<uint8_t>(0x40 + col),
  };
  write_bytes(cursor_seq, sizeof(cursor_seq));
}

void clear_line_buffer(uint8_t row) {
  if (row < 1 || row > UI_ROWS) {
    return;
  }
  memset(g_screen_buffer[row - 1], ' ', UI_COLS);
  g_screen_buffer[row - 1][UI_COLS] = '\0';
}

void initialize_screen_buffer(void) {
  for (uint8_t row = 1; row <= UI_ROWS; ++row) {
    clear_line_buffer(row);
  }
}

void initialize_sent_buffer(void) {
  for (uint8_t row = 0; row < UI_ROWS; ++row) {
    memset(g_sent_buffer[row], ' ', UI_COLS);
    g_sent_buffer[row][UI_COLS] = '\0';
  }
}

void clear_row_mosaic_overlay(row_mosaic_overlay_t* overlay) {
  overlay->active = false;
  overlay->start_col = 1;
  overlay->width = 0;
  memset(overlay->masks, 0, sizeof(overlay->masks));
}

void initialize_screen_mosaic_overlays(void) {
  for (uint8_t row = 0; row < UI_ROWS; ++row) {
    clear_row_mosaic_overlay(&g_screen_mosaic_overlays[row]);
  }
}

void initialize_sent_mosaic_overlays(void) {
  for (uint8_t row = 0; row < UI_ROWS; ++row) {
    clear_row_mosaic_overlay(&g_sent_mosaic_overlays[row]);
  }
}

bool mosaic_overlay_equals(const row_mosaic_overlay_t& left, const row_mosaic_overlay_t& right) {
  if (left.active != right.active) {
    return false;
  }
  if (!left.active) {
    return true;
  }
  if (left.start_col != right.start_col || left.width != right.width) {
    return false;
  }
  return memcmp(left.masks, right.masks, left.width) == 0;
}

bool alpha_row_equals_outside_mosaic(uint8_t row, const row_mosaic_overlay_t& overlay) {
  const uint8_t row_index = static_cast<uint8_t>(row - 1);
  if (!overlay.active) {
    return memcmp(g_screen_buffer[row_index], g_sent_buffer[row_index], UI_COLS) == 0;
  }

  const uint8_t start_index = static_cast<uint8_t>(overlay.start_col - 1);
  const uint8_t end_index = static_cast<uint8_t>(start_index + overlay.width);
  if (start_index > 0 &&
      memcmp(g_screen_buffer[row_index], g_sent_buffer[row_index], start_index) != 0) {
    return false;
  }
  if (end_index < UI_COLS &&
      memcmp(g_screen_buffer[row_index] + end_index,
             g_sent_buffer[row_index] + end_index,
             UI_COLS - end_index) != 0) {
    return false;
  }
  return true;
}

void write_buffered_text(uint8_t row, uint8_t col, const char* text) {
  if (row < 1 || row > UI_ROWS || col < 1 || col > UI_COLS || !text) {
    return;
  }

  uint8_t index = static_cast<uint8_t>(col - 1);
  while (*text && index < UI_COLS) {
    g_screen_buffer[row - 1][index++] =
        static_cast<char>(sanitize_char(static_cast<uint8_t>(*text++)));
  }
}

void draw_graphics_separator(uint8_t row) {
  uint8_t masks[UI_COLS];
  memset(masks, 0x30, sizeof(masks));
  ge_enter_graphics(UI_GRAPHICS_CONTIGUOUS);
  mosaic_write_run(static_cast<uint8_t>(row - 1), 0, masks, UI_COLS, GE_WHITE);
  ge_exit_graphics();
}

// Index just past the last non-space cell of a row (0 for a blank row).
uint8_t row_content_end(const char* cells) {
  uint8_t end = UI_COLS;
  while (end > 0 && cells[end - 1] == ' ') {
    --end;
  }
  return end;
}

void write_cells(const char* cells, uint8_t from, uint8_t to, int8_t* cursor_col) {
  for (uint8_t col = from; col < to; ++col) {
    uart_minitel_write(sanitize_char(static_cast<uint8_t>(cells[col])));
  }
  *cursor_col = to < UI_COLS ? static_cast<int8_t>(to) : -1;
}

// Moves the terminal cursor to the 0-based column `col` of `row`. When the cursor already
// sits a few cells to the left, rewriting those cells from `cells` beats a US sequence.
void move_cursor_in_row(uint8_t row, uint8_t col, const char* cells, int8_t* cursor_col) {
  if (*cursor_col >= 0 && col >= *cursor_col && col - *cursor_col <= UI_SEGMENT_MERGE_GAP) {
    write_cells(cells, static_cast<uint8_t>(*cursor_col), col, cursor_col);
    return;
  }
  set_cursor_impl(row, static_cast<uint8_t>(col + 1));
  *cursor_col = static_cast<int8_t>(col);
}

// Rewrites a whole row (needed when mosaic cells may be on screen): the content, then CAN
// to blank the rest of the row instead of sending trailing spaces.
void flush_alpha_full_row(uint8_t row) {
  const char* cells = g_screen_buffer[row - 1];
  const uint8_t content_end = row_content_end(cells);
  int8_t cursor_col = -1;
  set_cursor_impl(row, 1);
  write_cells(cells, 0, content_end, &cursor_col);
  if (content_end < UI_COLS) {
    uart_minitel_write(MT_CAN);
  }
  memcpy(g_sent_buffer[row - 1], cells, UI_COLS + 1);
}

void draw_mosaic_overlay(uint8_t row, const row_mosaic_overlay_t& overlay) {
  if (!overlay.active || overlay.width == 0) {
    return;
  }

  ge_enter_graphics(UI_GRAPHICS_CONTIGUOUS);
  mosaic_write_run(static_cast<uint8_t>(row - 1),
                   static_cast<uint8_t>(overlay.start_col - 1),
                   overlay.masks,
                   overlay.width,
                   GE_WHITE);
  ge_exit_graphics();
  memcpy(&g_sent_mosaic_overlays[row - 1], &overlay, sizeof(overlay));
  minitel_ui_alpha_mode();
}

void flush_alpha_row_segments(uint8_t row) {
  const char* target = g_screen_buffer[row - 1];
  const char* shown = g_sent_buffer[row - 1];
  if (memcmp(target, shown, UI_COLS) == 0) {
    return;
  }

  const uint8_t target_end = row_content_end(target);
  int8_t cursor_col = -1;  // 0-based terminal cursor column on this row, -1 when unknown
  uint8_t col = 0;
  while (col < target_end) {
    if (target[col] == shown[col]) {
      ++col;
      continue;
    }

    uint8_t run_end = col;
    while (run_end < target_end && target[run_end] != shown[run_end]) {
      ++run_end;
    }
    move_cursor_in_row(row, col, target, &cursor_col);
    write_cells(target, col, run_end, &cursor_col);
    col = run_end;
  }

  // The target is blank from target_end on: erase stale text there with a single CAN.
  if (row_content_end(shown) > target_end) {
    if (cursor_col != target_end) {
      move_cursor_in_row(row, target_end, target, &cursor_col);
    }
    uart_minitel_write(MT_CAN);
  }

  memcpy(g_sent_buffer[row - 1], target, UI_COLS + 1);
}

void flush_row_with_mosaic_overlay(uint8_t row) {
  const row_mosaic_overlay_t& screen_overlay = g_screen_mosaic_overlays[row - 1];
  const row_mosaic_overlay_t& sent_overlay = g_sent_mosaic_overlays[row - 1];

  if (mosaic_overlay_equals(screen_overlay, sent_overlay) &&
      alpha_row_equals_outside_mosaic(row, screen_overlay)) {
    return;
  }

  flush_alpha_full_row(row);
  if (screen_overlay.active) {
    draw_mosaic_overlay(row, screen_overlay);
  } else {
    clear_row_mosaic_overlay(&g_sent_mosaic_overlays[row - 1]);
  }
}

bool row_has_mosaic(uint8_t row) {
  return row != UI_SEPARATOR_ROW &&
         (g_screen_mosaic_overlays[row - 1].active || g_sent_mosaic_overlays[row - 1].active);
}

bool row_needs_flush(uint8_t row) {
  if (row_has_mosaic(row)) {
    const row_mosaic_overlay_t& overlay = g_screen_mosaic_overlays[row - 1];
    return !mosaic_overlay_equals(overlay, g_sent_mosaic_overlays[row - 1]) ||
           !alpha_row_equals_outside_mosaic(row, overlay);
  }
  return memcmp(g_screen_buffer[row - 1], g_sent_buffer[row - 1], UI_COLS) != 0;
}

bool rows_need_flush(uint8_t row_start, uint8_t row_end) {
  for (uint8_t row = row_start; row <= row_end; ++row) {
    if (row_needs_flush(row)) {
      return true;
    }
  }
  return false;
}

void flush_row(uint8_t row) {
  if (row == UI_SEPARATOR_ROW) {
    draw_graphics_separator(row);
    memcpy(g_sent_buffer[row - 1], g_screen_buffer[row - 1], UI_COLS + 1);
    minitel_ui_alpha_mode();
  } else if (row_has_mosaic(row)) {
    flush_row_with_mosaic_overlay(row);
  } else {
    flush_alpha_row_segments(row);
  }
}

// Every row update starts with a US, which resets the attributes and the character set: no
// mode sequence is needed before it. Returns false when it stopped early for a waiting key.
bool flush_rows_impl(uint8_t row_start, uint8_t row_end, bool allow_full_refresh, bool until_input) {
  if (row_start < 1) {
    row_start = 1;
  }
  if (row_end > UI_ROWS) {
    row_end = UI_ROWS;
  }
  if (row_start > row_end) {
    return true;
  }

  const bool full_refresh = allow_full_refresh && (g_force_full_refresh || !g_has_sent_buffer);
  if (!full_refresh && !rows_need_flush(row_start, row_end)) {
    return true;
  }

  minitel_ui_set_cursor_visible(false);  // it would wander over the page while drawing
  if (full_refresh) {
    // FF blanks the page, so diffing against a blank screen only sends actual content.
    clear_screen_impl();
    initialize_sent_buffer();
    initialize_sent_mosaic_overlays();
    g_force_full_refresh = false;
  }
  g_has_sent_buffer = true;

  for (uint8_t row = row_start; row <= row_end; ++row) {
    if (!row_needs_flush(row)) {
      continue;
    }
    flush_row(row);
    // A waiting key changes what to draw: it is handled first, the next flush sends the rest.
    if (until_input && row < row_end && uart_minitel_available() &&
        rows_need_flush(static_cast<uint8_t>(row + 1), row_end)) {
      return false;
    }
  }

  uart_minitel_flush();
  return true;
}
}  // namespace

void minitel_ui_begin(uint32_t baud, int8_t rx_pin, int8_t tx_pin) {
  uart_minitel_begin(baud, SERIAL_7E1, rx_pin, tx_pin);
  delay_ms(50);
  lm_bind_io(write_bytes, print_text, set_cursor_impl, clear_screen_impl);
  initialize_screen_buffer();
  initialize_sent_buffer();
  initialize_screen_mosaic_overlays();
  initialize_sent_mosaic_overlays();
  g_has_sent_buffer = false;
  g_force_full_refresh = true;
  g_cursor_state = CURSOR_UNKNOWN;
  g_last_flush_interrupted = false;
}

void minitel_ui_clear_screen(void) {
  clear_screen_impl();
  initialize_sent_buffer();
  initialize_sent_mosaic_overlays();
  g_has_sent_buffer = false;
  g_force_full_refresh = true;
  minitel_ui_alpha_mode();
}

void minitel_ui_alpha_mode(void) {
  uint8_t alpha_seq[2] = {0x1B, 'X'};
  write_bytes(alpha_seq, sizeof(alpha_seq));
  uart_minitel_write(0x0F);
}

void minitel_ui_render_topbar(const char* screen_name,
                              const char* wifi_label,
                              const char* time_label) {
  char line[UI_COLS + 1];
  snprintf(line, sizeof(line), "%s | %s | %s",
           screen_name ? screen_name : "SCREEN",
           wifi_label ? wifi_label : "offline",
           time_label ? time_label : "--:--");
  clear_line_buffer(UI_TOPBAR_ROW);
  write_buffered_text(UI_TOPBAR_ROW, 1, line);

  char separator[UI_COLS + 1];
  memset(separator, '=', UI_COLS);
  separator[UI_COLS] = '\0';
  clear_line_buffer(UI_SEPARATOR_ROW);
  write_buffered_text(UI_SEPARATOR_ROW, 1, separator);
}

void minitel_ui_render_frame(const char* screen_name,
                             const char* wifi_label,
                             const char* time_label,
                             const char* footer) {
  initialize_screen_buffer();
  initialize_screen_mosaic_overlays();
  minitel_ui_render_topbar(screen_name, wifi_label, time_label);
  write_buffered_text(UI_FOOTER_ROW, 1, footer ? footer : "");
}

void minitel_ui_clear_body(void) {
  for (uint8_t row = UI_CONTENT_START_ROW; row <= UI_CONTENT_END_ROW; ++row) {
    clear_line_buffer(row);
  }
}

void minitel_ui_write_line(uint8_t row, const char* text) {
  clear_line_buffer(row);
  write_buffered_text(row, 1, text);
}

void minitel_ui_write_at(uint8_t row, uint8_t col, const char* text) {
  write_buffered_text(row, col, text);
}

void minitel_ui_write_big_at(uint8_t row, uint8_t col, const char* text) {
  write_buffered_text(row, col, text);
}

void minitel_ui_write_big_line(uint8_t row, const char* text) {
  minitel_ui_write_line(row, text);
}

void minitel_ui_draw_big_cursor(uint8_t /*row*/, uint8_t /*col*/) {
}

void minitel_ui_flush(void) {
  flush_rows_impl(1, UI_ROWS, true, false);
  g_last_flush_interrupted = false;
}

bool minitel_ui_flush_until_input(void) {
  const bool complete = flush_rows_impl(1, UI_ROWS, true, !g_last_flush_interrupted);
  g_last_flush_interrupted = !complete;
  return complete;
}

void minitel_ui_flush_rows(uint8_t row_start, uint8_t row_end) {
  flush_rows_impl(row_start, row_end, false, false);
}

void minitel_ui_force_full_refresh(void) {
  g_force_full_refresh = true;
  g_has_sent_buffer = false;
  initialize_sent_buffer();
  initialize_sent_mosaic_overlays();
  g_cursor_state = CURSOR_UNKNOWN;  // a restarted Minitel shows it off whatever we sent
}

void minitel_ui_set_cursor(uint8_t row, uint8_t col) {
  set_cursor_impl(row, col);
}

void minitel_ui_set_cursor_visible(bool visible) {
  const cursor_state_t state = visible ? CURSOR_SHOWN : CURSOR_HIDDEN;
  if (g_cursor_state == state) {
    return;
  }
  uart_minitel_write(visible ? 0x11 : 0x14);
  g_cursor_state = state;
}

void minitel_ui_draw_mosaic_art(uint8_t row,
                                uint8_t col,
                                const uint8_t* masks,
                                uint8_t width,
                                uint8_t height) {
  if (!masks || width == 0 || height == 0 || row < 1 || row > UI_ROWS || col < 1 || col > UI_COLS) {
    return;
  }

  uint8_t draw_width = width;
  if (static_cast<uint16_t>(col) + width - 1 > UI_COLS) {
    draw_width = static_cast<uint8_t>(UI_COLS - col + 1);
  }

  ge_enter_graphics(UI_GRAPHICS_CONTIGUOUS);
  for (uint8_t art_row = 0; art_row < height; ++art_row) {
    const uint8_t screen_row = static_cast<uint8_t>(row + art_row);
    if (screen_row > UI_ROWS) {
      break;
    }
    row_mosaic_overlay_t& overlay = g_screen_mosaic_overlays[screen_row - 1];
    overlay.active = true;
    overlay.start_col = col;
    overlay.width = draw_width;
    memcpy(overlay.masks, masks + (art_row * width), draw_width);
    if (!mosaic_overlay_equals(overlay, g_sent_mosaic_overlays[screen_row - 1])) {
      mosaic_write_run(static_cast<uint8_t>(screen_row - 1),
                       static_cast<uint8_t>(col - 1),
                       overlay.masks,
                       draw_width,
                       GE_WHITE);
      memcpy(&g_sent_mosaic_overlays[screen_row - 1], &overlay, sizeof(overlay));
    }
  }
  ge_exit_graphics();
  uart_minitel_flush();
}

void minitel_ui_beep(uint8_t count, uint16_t gap_ms) {
  for (uint8_t i = 0; i < count; ++i) {
    uart_minitel_write(MT_BELL);
    uart_minitel_flush();
    if (gap_ms > 0 && (i + 1) < count) {
      delay_ms(gap_ms);
    }
  }
}
