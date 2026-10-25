#ifndef APP_SCREENS_SEARCH_SCREEN_H
#define APP_SCREENS_SEARCH_SCREEN_H

#include "app/input.h"

// Full-text search in every note and project (SCREEN_SEARCH); ENVOI opens the document
// on the matching line.
void search_screen_start();
void search_screen_render();
void search_screen_handle(const input_event_t& event);

#endif
