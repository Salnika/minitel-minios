#ifndef APP_SCREENS_LOCK_SCREEN_H
#define APP_SCREENS_LOCK_SCREEN_H

#include "app/input.h"

// PIN asked at boot when one is set in Reglages > Code PIN (SCREEN_LOCK).
bool lock_screen_required();
void lock_screen_open();
void lock_screen_render();
void lock_screen_handle(const input_event_t& event);
// From the settings input: 4 to 8 digits, or empty to remove the PIN.
void lock_screen_change_pin(const char* pin);

#endif
