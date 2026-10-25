#include "lib_minitel/gfx/mosaic_map.h"
#include "lib_minitel/gfx/graphics_engine.h"

// Stored masks for each 40x25 character cell (6-bit per cell)
static uint8_t g_masks[25][40];
static bool g_contig = true; // true=Z (contiguous), false=Y (separated)
static ge_color_t g_color = GE_WHITE; // current drawing color for flushes

static inline uint8_t code_from_mask(uint8_t mask)
{
    // Telic VB overrides for common primitives; fallback to 0x20 + mask
    switch (mask & 0x3F) {
        case 0x03: return 0x23; // top row
        case 0x2A: return 0x4A; // right col
        case 0x30: return 0x50; // bottom row
        case 0x15: return 0x35; // left col
        default:   return (uint8_t)(0x20 + (mask & 0x3F));
    }
}

static inline void assert_contiguity(void)
{
    ge_set_mosaic_contiguous(g_contig);
}

void mosaic_init(void)
{
    for (int r = 0; r < 25; ++r)
        for (int c = 0; c < 40; ++c)
            g_masks[r][c] = 0;
    g_contig = true;
}

void mosaic_set_mode(bool contiguous)
{
    g_contig = contiguous;
}

void mosaic_clear(void)
{
    for (int r = 0; r < 25; ++r)
        for (int c = 0; c < 40; ++c)
            g_masks[r][c] = 0;
}

void mosaic_set_color(ge_color_t color)
{
    g_color = color;
}

static inline void flush_cell(uint8_t cell_row, uint8_t cell_col)
{
    if (cell_row >= 25 || cell_col >= 40) return;
    uint8_t mask = g_masks[cell_row][cell_col] & 0x3F;
    uint8_t code = code_from_mask(mask);

    ge_enter_graphics(g_contig);
    ge_set_cursor((uint8_t)(cell_row + 1), (uint8_t)(cell_col + 1));
    assert_contiguity();
    // Apply current color before writing cell
    ge_set_color(g_color);
    ge_write_cells(&code, 1);
    ge_exit_graphics();
}

bool mosaic_set_pixel(uint8_t x, uint8_t y, bool on)
{
    if (x >= 80 || y >= 75) return false;
    uint8_t cell_col = (uint8_t)(x >> 1);          // /2
    uint8_t cell_row = (uint8_t)(y / 3);           // /3
    uint8_t sub_c = (uint8_t)(x & 1);              // 0..1
    uint8_t sub_r = (uint8_t)(y - (cell_row * 3)); // 0..2
    uint8_t bit = (uint8_t)(sub_r * 2 + sub_c);    // 0..5

    uint8_t old_mask = g_masks[cell_row][cell_col] & 0x3F;
    uint8_t new_mask = old_mask;
    if (on) new_mask |=  (1 << bit);
    else    new_mask &= ~(1 << bit);
    if (new_mask == old_mask) return false;

    g_masks[cell_row][cell_col] = new_mask;
    flush_cell(cell_row, cell_col);
    return true;
}

void mosaic_set_cell_mask(uint8_t cell_row, uint8_t cell_col, uint8_t mask)
{
    if (cell_row >= 25 || cell_col >= 40) return;
    g_masks[cell_row][cell_col] = (mask & 0x3F);
    flush_cell(cell_row, cell_col);
}

uint8_t mosaic_get_cell_mask(uint8_t cell_row, uint8_t cell_col)
{
    if (cell_row >= 25 || cell_col >= 40) return 0;
    return (g_masks[cell_row][cell_col] & 0x3F);
}

void mosaic_write_run(uint8_t cell_row,
                      uint8_t cell_col,
                      const uint8_t* masks,
                      uint8_t len,
                      ge_color_t color)
{
    if (!masks || len == 0 || cell_row >= 25 || cell_col >= 40) {
        return;
    }

    if ((uint16_t)cell_col + len > 40) {
        len = (uint8_t)(40 - cell_col);
    }

    uint8_t codes[40];
    for (uint8_t i = 0; i < len; ++i) {
        codes[i] = code_from_mask(masks[i] & 0x3F);
    }

    ge_set_cursor((uint8_t)(cell_row + 1), (uint8_t)(cell_col + 1));
    ge_set_color(color);
    ge_write_cells(codes, len);
}
