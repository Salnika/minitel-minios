#include "lib_minitel/gfx/graphics_engine.h"

static ge_write_bytes_fn _write = 0;
static ge_print_text_fn  _print = 0;
static ge_set_cursor_fn  _cursor = 0;
static ge_clear_fn       _clear = 0;

static bool _invert = false;
static bool _gfx = false;

void ge_bind(ge_write_bytes_fn write_fn, ge_print_text_fn print_fn, ge_set_cursor_fn cursor_fn, ge_clear_fn clear_fn) {
    _write = write_fn; _print = print_fn; _cursor = cursor_fn; _clear = clear_fn;
}

void ge_clear(void) { if (_clear) _clear(); }
void ge_set_cursor(uint8_t row, uint8_t col) { if (_cursor) _cursor(row, col); }
void ge_print(const char* s) { if (_print) _print(s); }


static void ge_send_esc(char c) {
    if (_write) { uint8_t seq[2] = {0x1B, (uint8_t)c}; _write(seq, 2); }
}

void ge_set_mosaic_contiguous(bool on) {
    // à appeler quand G1 est actif (après SO)
    ge_send_esc(on ? 'Z' : 'Y');   // Z = contigu, Y = séparé
}

void ge_enter_graphics(bool contiguous) {
    uint8_t SI = 0x0F;               // repasse G0 propre
    if (_write) _write(&SI, 1);
    ge_send_esc('@');                 // reset neutre

    uint8_t SO = 0x0E;                // active G1 (mosaïque)
    if (_write) _write(&SO, 1);
    _gfx = true;

    ge_set_mosaic_contiguous(contiguous);   // ⚠️ contigu APRES SO, pour Telic/Alcatel
}

void ge_exit_graphics(void) {
    uint8_t SI = 0x0F;                // retour G0
    if (_write) _write(&SI, 1);
    ge_send_esc('X');                 // alpha
    _gfx = false;
}


void ge_set_color(ge_color_t c) {
    switch (c) {
        case GE_WHITE:   ge_send_esc('P'); break;
        case GE_RED:     ge_send_esc('Q'); break;
        case GE_GREEN:   ge_send_esc('R'); break;
        case GE_BLUE:    ge_send_esc('S'); break;
        case GE_YELLOW:  ge_send_esc('T'); break;
        case GE_MAGENTA: ge_send_esc('U'); break;
        case GE_CYAN:    ge_send_esc('V'); break;
        default: break;
    }
}

void ge_invert_set(bool on) { _invert = on; }
void ge_invert_toggle(void) { _invert = !_invert; }
bool ge_invert_get(void) { return _invert; }

uint8_t ge_mz(uint8_t code) {
    if (!_invert) return code;
    if (code >= 0x40 && code <= 0x5F) return (uint8_t)(code + 0x20);
    if (code >= 0x60 && code <= 0x7F) return (uint8_t)(code - 0x20);
    return code;
}

void ge_newline_graphics(void) {
    if (!_write) return;
    uint8_t nl[2] = { '\r', '\n' };
    _write(nl, 2);
    if (_gfx) { uint8_t SO = 0x0E; _write(&SO, 1); }
}

void ge_write_cells(const uint8_t* cells, size_t len) {
    if (!_write || !cells || !len) return;
    if (_gfx) { uint8_t SO = 0x0E; _write(&SO, 1); }
    uint8_t buf[64];
    size_t i = 0;
    while (i < len) {
        size_t chunk = (len - i > sizeof(buf)) ? sizeof(buf) : (len - i);
        for (size_t k = 0; k < chunk; ++k) buf[k] = ge_mz(cells[i + k]);
        _write(buf, chunk);
        i += chunk;
    }
}

void ge_fill_row(uint8_t cell_code, uint8_t count) {
    if (!_write || count == 0) return;
    if (_gfx) { uint8_t SO = 0x0E; _write(&SO, 1); }
    uint8_t c = ge_mz(cell_code);
    for (uint8_t i = 0; i < count; ++i) _write(&c, 1);
}
