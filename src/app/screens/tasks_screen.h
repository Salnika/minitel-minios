#ifndef APP_SCREENS_TASKS_SCREEN_H
#define APP_SCREENS_TASKS_SCREEN_H

#include "app/input.h"

// Tasks due today or late across every project (SCREEN_TASKS); T switches to all open tasks.
void tasks_screen_open();
void tasks_screen_render();
void tasks_screen_handle(const input_event_t& event);
// Back from the editor: the task files may have changed.
void tasks_screen_resume();

#endif
