#ifndef GRAPHICS_ENGINE_H
#define GRAPHICS_ENGINE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*ge_write_bytes_fn)(const uint8_t* data, size_t len);
typedef void (*ge_print_text_fn)(const char* s);
typedef void (*ge_set_cursor_fn)(uint8_t row, uint8_t col);
typedef void (*ge_clear_fn)(void);

typedef enum {
    GE_WHITE = 0,
    GE_RED,
    GE_GREEN,
    GE_BLUE,
    GE_YELLOW,
    GE_MAGENTA,
    GE_CYAN
} ge_color_t;

void ge_bind(ge_write_bytes_fn write_fn, ge_print_text_fn print_fn, ge_set_cursor_fn cursor_fn, ge_clear_fn clear_fn);

void ge_clear(void);
void ge_set_cursor(uint8_t row, uint8_t col);
void ge_print(const char* s);

void ge_enter_graphics(bool contiguous);
void ge_exit_graphics(void);
void ge_set_mosaic_contiguous(bool on);

void ge_set_color(ge_color_t c);

void ge_invert_set(bool on);
void ge_invert_toggle(void);
bool ge_invert_get(void);

uint8_t ge_mz(uint8_t code);
void ge_write_cells(const uint8_t* cells, size_t len);
void ge_fill_row(uint8_t cell_code, uint8_t count);

#ifdef __cplusplus
}
#endif

#endif
