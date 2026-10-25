#ifndef LIB_MINITEL_MINIGFX_H
#define LIB_MINITEL_MINIGFX_H
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LM_WHITE = 0,
    LM_RED,
    LM_GREEN,
    LM_BLUE,
    LM_YELLOW,
    LM_MAGENTA,
    LM_CYAN
} lm_color_t;

void lm_gfx_clear(void);
void lm_gfx_enter(bool contiguous);
void lm_gfx_exit(void);
void lm_gfx_set_color(lm_color_t c);
void lm_gfx_set_contiguity(bool contiguous);

// 80x75 logical pixel space helpers (2x3 per char)
void lm_gfx_set_mode(bool contiguous);
void lm_gfx_pixel(uint8_t x, uint8_t y, bool on);
void lm_gfx_set_cell_mask(uint8_t row, uint8_t col, uint8_t mask);
uint8_t lm_gfx_get_cell_mask(uint8_t row, uint8_t col);
void lm_gfx_clear_pixels(void);

// Simple drawing utils
void lm_draw_line(int x0, int y0, int x1, int y1, bool on);
void lm_draw_rect(int x, int y, int w, int h, bool on);
void lm_fill_rect(int x, int y, int w, int h, bool on);
void lm_draw_circle(int cx, int cy, int r, bool on);

#ifdef __cplusplus
}
#endif
#endif
