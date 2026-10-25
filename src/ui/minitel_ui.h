#ifndef UI_MINITEL_UI_H
#define UI_MINITEL_UI_H

#include <stdint.h>

// Videotex page rows 1-24 (row 0 is the terminal service line, US cannot address row 25).
constexpr uint8_t UI_ROWS = 24;
constexpr uint8_t UI_COLS = 40;
constexpr uint8_t UI_PIXEL_ROWS = UI_ROWS * 3;
constexpr uint8_t UI_PIXEL_COLS = UI_COLS * 2;
constexpr uint8_t UI_TOPBAR_ROW = 1;
constexpr uint8_t UI_SEPARATOR_ROW = 2;
constexpr uint8_t UI_CONTENT_START_ROW = 3;
constexpr uint8_t UI_CONTENT_END_ROW = 22;
constexpr uint8_t UI_STATUS_ROW = 23;
constexpr uint8_t UI_FOOTER_ROW = 24;
constexpr uint8_t UI_BIG_CHAR_ROWS = 2;
constexpr uint8_t UI_BIG_CHAR_COLS = 2;
constexpr uint8_t UI_BIG_CHAR_HEIGHT = 6;
constexpr uint8_t UI_BIG_CHAR_WIDTH = 4;

void minitel_ui_begin(uint32_t baud, int8_t rx_pin, int8_t tx_pin);
void minitel_ui_clear_screen(void);
void minitel_ui_alpha_mode(void);
void minitel_ui_render_frame(const char* screen_name,
                             const char* wifi_label,
                             const char* time_label,
                             const char* footer);
void minitel_ui_render_topbar(const char* screen_name,
                              const char* wifi_label,
                              const char* time_label);
void minitel_ui_clear_body(void);
void minitel_ui_write_line(uint8_t row, const char* text);
void minitel_ui_write_at(uint8_t row, uint8_t col, const char* text);
void minitel_ui_flush(void);
// Like minitel_ui_flush, but stops between rows when a key is waiting (it changes what to
// draw anyway) and returns false: the next flush sends the rest. Never stops twice in a row.
bool minitel_ui_flush_until_input(void);
void minitel_ui_flush_rows(uint8_t row_start, uint8_t row_end);
void minitel_ui_force_full_refresh(void);
void minitel_ui_set_cursor(uint8_t row, uint8_t col);
void minitel_ui_set_cursor_visible(bool visible);
void minitel_ui_write_big_at(uint8_t row, uint8_t col, const char* text);
void minitel_ui_write_big_line(uint8_t row, const char* text);
void minitel_ui_draw_big_cursor(uint8_t row, uint8_t col);
void minitel_ui_draw_mosaic_art(uint8_t row,
                                uint8_t col,
                                const uint8_t* masks,
                                uint8_t width,
                                uint8_t height);
void minitel_ui_beep(uint8_t count = 1, uint16_t gap_ms = 0);

#endif
