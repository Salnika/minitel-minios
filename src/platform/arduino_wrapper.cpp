#include <Arduino.h>
#include <HardwareSerial.h>
#include <stdio.h>
#include <atomic>
#include "arduino_wrapper.h"
#include "app_log.h"

extern "C"
{
    namespace
    {
        constexpr size_t MINITEL_ECHO_BUFFER_SIZE = 2048;
        constexpr uint32_t MINITEL_ECHO_SETTLE_MS = 25;
        constexpr uint32_t MINITEL_ECHO_SPACE_LOG_INTERVAL_MS = 250;
        constexpr uint32_t MINITEL_ECHO_CANDIDATE_HOLD_MS = 18;
        constexpr uint32_t MINITEL_ECHO_RUN_IDLE_MS = 30;
        HardwareSerial *minitel_serial = nullptr;
        uint8_t minitel_echo_buffer[MINITEL_ECHO_BUFFER_SIZE];
        size_t minitel_echo_head = 0;
        size_t minitel_echo_tail = 0;
        uint32_t minitel_last_echo_space_log_ms = 0;
        bool minitel_input_staged_valid = false;
        uint8_t minitel_input_staged_value = 0;
        bool minitel_echo_candidate_valid = false;
        uint8_t minitel_echo_candidate_value = 0;
        uint32_t minitel_echo_candidate_ms = 0;
        bool minitel_echo_run_active = false;
        uint32_t minitel_echo_run_last_ms = 0;
        bool minitel_echo_filter = true;
        // Set from the UART event task.
        std::atomic<bool> minitel_rx_error{false};

        size_t minitel_echo_next(size_t index)
        {
            return (index + 1U) % MINITEL_ECHO_BUFFER_SIZE;
        }

        bool minitel_echo_is_empty()
        {
            return minitel_echo_head == minitel_echo_tail;
        }

        void minitel_echo_push(uint8_t value)
        {
            size_t next_head = minitel_echo_next(minitel_echo_head);
            if (next_head == minitel_echo_tail)
            {
                minitel_echo_tail = minitel_echo_next(minitel_echo_tail);
            }
            minitel_echo_buffer[minitel_echo_head] = value;
            minitel_echo_head = next_head;
        }

        bool minitel_echo_matches(uint8_t value)
        {
            return !minitel_echo_is_empty() && minitel_echo_buffer[minitel_echo_tail] == value;
        }

        void minitel_echo_pop()
        {
            if (!minitel_echo_is_empty())
            {
                minitel_echo_tail = minitel_echo_next(minitel_echo_tail);
            }
        }

        void minitel_log_discarded_echo_byte(uint8_t value)
        {
            if (value != 0x20)
            {
                return;
            }

            const uint32_t now = millis();
            if (now - minitel_last_echo_space_log_ms >= MINITEL_ECHO_SPACE_LOG_INTERVAL_MS)
            {
                app_log_info("uart-echo", "discard echoed space before input parser");
                minitel_last_echo_space_log_ms = now;
            }
        }

        bool minitel_is_printable_echo_candidate(uint8_t value)
        {
            return (value >= 0x20 && value <= 0x7E) || value == 0x00 || value == 0xA0;
        }

        void minitel_stage_input(uint8_t value)
        {
            minitel_input_staged_value = value;
            minitel_input_staged_valid = true;
        }

        void minitel_clear_echo_candidate()
        {
            minitel_echo_candidate_valid = false;
            minitel_echo_candidate_value = 0;
            minitel_echo_candidate_ms = 0;
        }

        void minitel_reset_input_state()
        {
            minitel_echo_head = 0;
            minitel_echo_tail = 0;
            minitel_input_staged_valid = false;
            minitel_input_staged_value = 0;
            minitel_clear_echo_candidate();
            minitel_echo_run_active = false;
            minitel_echo_run_last_ms = 0;
        }

        void minitel_on_receive_error(hardwareSerial_error_t error)
        {
            if (error == UART_FRAME_ERROR || error == UART_PARITY_ERROR || error == UART_BREAK_ERROR)
            {
                minitel_rx_error.store(true);
            }
        }

        void minitel_discard_confirmed_echo_byte(void)
        {
            if (!minitel_serial || !minitel_echo_candidate_valid || minitel_serial->available() <= 0)
            {
                return;
            }

            int next_value = minitel_serial->read();
            if (next_value < 0)
            {
                return;
            }

            minitel_log_discarded_echo_byte(minitel_echo_candidate_value);
            minitel_log_discarded_echo_byte(static_cast<uint8_t>(next_value));
            minitel_echo_pop();
            minitel_clear_echo_candidate();
            minitel_echo_run_active = true;
            minitel_echo_run_last_ms = millis();
        }

        void minitel_prepare_input()
        {
            if (!minitel_serial || minitel_input_staged_valid)
            {
                return;
            }

            if (!minitel_echo_filter)
            {
                if (minitel_serial->available() > 0)
                {
                    minitel_stage_input(static_cast<uint8_t>(minitel_serial->read()));
                }
                return;
            }

            if (minitel_echo_run_active &&
                millis() - minitel_echo_run_last_ms > MINITEL_ECHO_RUN_IDLE_MS)
            {
                minitel_echo_run_active = false;
            }

            while (!minitel_input_staged_valid)
            {
                const uint32_t now = millis();

                if (minitel_echo_candidate_valid)
                {
                    if (minitel_serial->available() <= 0)
                    {
                        if (now - minitel_echo_candidate_ms >= MINITEL_ECHO_CANDIDATE_HOLD_MS)
                        {
                            minitel_stage_input(minitel_echo_candidate_value);
                            minitel_clear_echo_candidate();
                        }
                        return;
                    }

                    int next_value = minitel_serial->peek();
                    if (next_value >= 0 &&
                        !minitel_echo_is_empty() &&
                        minitel_echo_matches(static_cast<uint8_t>(next_value)))
                    {
                        minitel_discard_confirmed_echo_byte();
                        continue;
                    }

                    minitel_stage_input(minitel_echo_candidate_value);
                    minitel_clear_echo_candidate();
                    return;
                }

                if (minitel_echo_run_active)
                {
                    if (minitel_serial->available() <= 0)
                    {
                        return;
                    }

                    int next_value = minitel_serial->peek();
                    if (next_value >= 0 &&
                        !minitel_echo_is_empty() &&
                        minitel_echo_matches(static_cast<uint8_t>(next_value)))
                    {
                        minitel_log_discarded_echo_byte(static_cast<uint8_t>(next_value));
                        minitel_serial->read();
                        minitel_echo_pop();
                        minitel_echo_run_last_ms = now;
                        continue;
                    }

                    minitel_echo_run_active = false;
                    continue;
                }

                if (minitel_serial->available() <= 0)
                {
                    return;
                }

                int next_value = minitel_serial->peek();
                if (next_value < 0)
                {
                    return;
                }

                const uint8_t value = static_cast<uint8_t>(next_value);
                if (!minitel_echo_is_empty() && minitel_echo_matches(value))
                {
                    if (minitel_is_printable_echo_candidate(value))
                    {
                        minitel_serial->read();
                        minitel_echo_pop();
                        minitel_echo_candidate_valid = true;
                        minitel_echo_candidate_value = value;
                        minitel_echo_candidate_ms = now;
                        continue;
                    }

                    minitel_log_discarded_echo_byte(value);
                    minitel_serial->read();
                    minitel_echo_pop();
                    minitel_echo_run_active = true;
                    minitel_echo_run_last_ms = now;
                    continue;
                }

                minitel_stage_input(static_cast<uint8_t>(minitel_serial->read()));
                return;
            }
        }

        void minitel_discard_echo()
        {
            if (!minitel_serial)
            {
                return;
            }

            while (!minitel_echo_is_empty() && minitel_serial->available() > 0)
            {
                int next_value = minitel_serial->peek();
                if (next_value < 0 || !minitel_echo_matches(static_cast<uint8_t>(next_value)))
                {
                    break;
                }

                minitel_log_discarded_echo_byte(static_cast<uint8_t>(next_value));
                minitel_serial->read();
                minitel_echo_pop();
            }
        }

        void minitel_drain_recent_echo()
        {
            if (!minitel_serial || minitel_echo_is_empty())
            {
                return;
            }

            const uint32_t start_ms = millis();
            while (!minitel_echo_is_empty())
            {
                minitel_discard_echo();
                if (minitel_echo_is_empty())
                {
                    return;
                }

                if (minitel_serial->available() > 0)
                {
                    return;
                }

                if (millis() - start_ms >= MINITEL_ECHO_SETTLE_MS)
                {
                    return;
                }

                delay(1);
            }
        }
    } // namespace
    // Serial debug wrapper
    void serial_begin(uint32_t baud)
    {
        Serial.begin(baud);
    }

    void serial_println(const char *str)
    {
        Serial.println(str);
    }

    void serial_printf(const char *format, ...)
    {
        char buf[256];
        va_list args;
        va_start(args, format);
        vsnprintf(buf, sizeof(buf), format, args);
        va_end(args);
        Serial.print(buf);
    }

    // Minitel UART wrapper
    void uart_minitel_begin(uint32_t baud, uint32_t /*config*/, int8_t rx_pin, int8_t tx_pin)
    {
        // Force the correct 7E1 configuration regardless of caller
        minitel_serial = &Serial1;
        minitel_echo_filter = true;
        minitel_reset_input_state();
        minitel_last_echo_space_log_ms = 0;
        minitel_rx_error.store(false);
        minitel_serial->begin(baud, SERIAL_7E1, rx_pin, tx_pin);
        minitel_serial->onReceiveError(minitel_on_receive_error);
    }

    void uart_minitel_set_baud(uint32_t baud)
    {
        if (!minitel_serial)
        {
            return;
        }
        minitel_serial->flush();
        minitel_serial->updateBaudRate(baud);
        uart_minitel_discard_input();
    }

    void uart_minitel_set_echo_filter(bool enabled)
    {
        minitel_echo_filter = enabled;
        minitel_reset_input_state();
    }

    void uart_minitel_discard_input(void)
    {
        if (minitel_serial)
        {
            while (minitel_serial->available() > 0)
            {
                minitel_serial->read();
            }
        }
        minitel_reset_input_state();
        minitel_rx_error.store(false);
    }

    bool uart_minitel_rx_error(void)
    {
        return minitel_rx_error.load();
    }

    void uart_minitel_flush(void)
    {
        if (minitel_serial)
        {
            minitel_serial->flush();
            minitel_drain_recent_echo();
        }
    }

    void uart_minitel_write(uint8_t c)
    {
        if (minitel_serial)
        {
            minitel_serial->write(c);
            if (minitel_echo_filter)
            {
                minitel_echo_push(c);
            }
        }
    }

    bool uart_minitel_available(void)
    {
        minitel_prepare_input();
        return minitel_input_staged_valid;
    }

    uint8_t uart_minitel_read(void)
    {
        minitel_prepare_input();
        if (minitel_input_staged_valid)
        {
            minitel_input_staged_valid = false;
            return minitel_input_staged_value;
        }
        return 0;
    }

    // Timing functions
    uint32_t get_millis(void)
    {
        return millis();
    }

    void delay_ms(uint32_t ms)
    {
        delay(ms);
    }

} // extern "C"

// Arduino framework entry points - allow disabling when another sketch defines them
#ifndef ARDUINO_WRAPPER_NO_SKETCH
void setup()
{
    app_setup();
}

void loop()
{
    app_loop();
}
#endif
