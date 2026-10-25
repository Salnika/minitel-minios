#ifndef APP_SCREENS_PROJECTS_SCREEN_H
#define APP_SCREENS_PROJECTS_SCREEN_H

#include "app/input.h"

// Project list (SCREEN_PROJECTS) and one project with its sheet, open tasks and timer
// (SCREEN_PROJECT). Data lives in Bibliotek under projets/<name>/.
void projects_screen_open();
void projects_screen_render();
void projects_screen_handle(const input_event_t& event);

void project_screen_render();
void project_screen_handle(const input_event_t& event);
// Back from the editor: the task files may have changed.
void project_screen_resume();

#endif
