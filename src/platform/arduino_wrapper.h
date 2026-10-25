#ifndef ARDUINO_WRAPPER_H
#define ARDUINO_WRAPPER_H

#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

// Serial debug functions
void serial_begin(uint32_t baud);
void serial_println(const char* str);
void serial_printf(const char* format, ...);

// Minitel UART functions
void uart_minitel_begin(uint32_t baud, uint32_t config, int8_t rx_pin, int8_t tx_pin);
// Sends what is queued at the old speed, then switches and drops pending input.
void uart_minitel_set_baud(uint32_t baud);
// On: every byte sent is expected back and filtered out of the input (Minitel echo).
void uart_minitel_set_echo_filter(bool enabled);
// Drops pending input and clears the receive error flag.
void uart_minitel_discard_input(void);
// A byte arrived with a framing or parity error since the last discard: the Minitel may
// talk at another speed (it restarted at 1200 baud).
bool uart_minitel_rx_error(void);
void uart_minitel_flush(void);
void uart_minitel_write(uint8_t c);
bool uart_minitel_available(void);
uint8_t uart_minitel_read(void);

// Timing functions
uint32_t get_millis(void);
void delay_ms(uint32_t ms);

// Main application functions
void app_setup(void);
void app_loop(void);


#ifdef __cplusplus
}
#endif

#endif // ARDUINO_WRAPPER_H
