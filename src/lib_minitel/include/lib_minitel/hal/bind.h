#ifndef LIB_MINITEL_BIND_H
#define LIB_MINITEL_BIND_H
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef void (*lm_write_bytes_fn)(const uint8_t* data, size_t len);
typedef void (*lm_print_text_fn)(const char* s);
typedef void (*lm_set_cursor_fn)(uint8_t row, uint8_t col);
typedef void (*lm_clear_fn)(void);

// Bind low-level I/O used by the graphics engine
void lm_bind_io(lm_write_bytes_fn write_fn, lm_print_text_fn print_fn,
                lm_set_cursor_fn cursor_fn, lm_clear_fn clear_fn);

#ifdef __cplusplus
}
#endif
#endif
