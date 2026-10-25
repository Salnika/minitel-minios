#ifndef APP_SCREENS_SYSTEM_SCREEN_H
#define APP_SCREENS_SYSTEM_SCREEN_H

#include "app/input.h"

// Device health (firmware, memory, uptime, network) plus updates from the server and
// restart, each confirmed by a second ENVOI.
void system_screen_open();
void system_screen_render();
void system_screen_handle(const input_event_t& event);
// Progress callback for updates (network push or server download): status line only.
void system_screen_show_update_progress(const char* message, int percent);

#endif
