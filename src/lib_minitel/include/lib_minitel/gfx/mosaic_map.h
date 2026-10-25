#ifndef MOSAIC_MAP_H
#define MOSAIC_MAP_H

#include <stdint.h>
#include <stdbool.h>
#include "graphics_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

// Initializes internal 40x25 mask buffer and sets default contiguous mode (Z)
void mosaic_init(void);

// Sets contiguity: true = contiguous (ESC 'Z'), false = separated (ESC 'Y')
void mosaic_set_mode(bool contiguous);

// Clears all stored masks (does not clear screen)
void mosaic_clear(void);

// Set the color used when flushing cells (applies before each write)
void mosaic_set_color(ge_color_t color);

// Sets or clears a logical subpixel at 80x75 space and flushes the containing cell
// x: 0..79, y: 0..74, on: true=set, false=clear
// Returns true if the underlying cell mask changed and was flushed
bool mosaic_set_pixel(uint8_t x, uint8_t y, bool on);

// Directly set a cell mask (6-bit) at cell coordinates (row:0..24, col:0..39) and flush
void mosaic_set_cell_mask(uint8_t cell_row, uint8_t cell_col, uint8_t mask);

// Get current stored mask for a cell (6-bit)
uint8_t mosaic_get_cell_mask(uint8_t cell_row, uint8_t cell_col);

// Write a run of masks starting at a cell without touching the internal stored buffer
void mosaic_write_run(uint8_t cell_row,
                      uint8_t cell_col,
                      const uint8_t* masks,
                      uint8_t len,
                      ge_color_t color);

#ifdef __cplusplus
}
#endif

#endif // MOSAIC_MAP_H
