#include "lib_minitel/hal/bind.h"
#include "lib_minitel/gfx/graphics_engine.h"

extern "C" {
void lm_bind_io(lm_write_bytes_fn write_fn, lm_print_text_fn print_fn,
                lm_set_cursor_fn cursor_fn, lm_clear_fn clear_fn) {
    ge_bind((ge_write_bytes_fn)write_fn, (ge_print_text_fn)print_fn,
            (ge_set_cursor_fn)cursor_fn, (ge_clear_fn)clear_fn);
}
}
