#ifndef APP_APP_SHELL_H
#define APP_APP_SHELL_H

#include <stdint.h>

#include "input.h"
#include "net_requests.h"

class ClockService;
class OtaService;
class SettingsService;
class WifiService;
struct app_settings_t;

// Screens of the miniOS. Feature screens live in src/app/screens/ and only talk to the
// shell API below; app.cpp owns navigation, the status line and text input. Network calls
// go through net::submit() (net_requests.h): they never block the screen.
enum screen_id_t {
  SCREEN_BOOT = 0,
  SCREEN_HOME,
  SCREEN_FILES,
  SCREEN_WIFI,
  SCREEN_SETTINGS,
  SCREEN_CHAT,
  SCREEN_EDITOR,
  SCREEN_TEXT_INPUT,
  SCREEN_MODEL_PICKER,
  SCREEN_SYSTEM,
  SCREEN_PROJECTS,
  SCREEN_PROJECT,
  SCREEN_TASKS,
  SCREEN_SEARCH,
  SCREEN_LOCK
};

// Receives the validated text; `value` is only valid during the call.
typedef void (*text_input_done_t)(const char* value);

namespace shell {

void set_status(const char* message);
void clear_status();
// Shows `message` right away, when the loop will not come back to draw it (before a restart).
void show_busy_status(const char* message);
void switch_screen(screen_id_t screen);
screen_id_t current_screen();
// Screen a request submitted now belongs to: the current one, or the one a text input
// returns to.
screen_id_t request_owner();
void request_redraw();
// Starts a frame: clears the buffer, draws the top bar (row 1) and `footer` (row 24).
// Screens then fill rows 3-22 and call render_status_line().
void render_frame(const char* screen_name, const char* footer);
// Row 23: the pending request of this screen with its timer, else the last status.
void render_status_line();
// Opens the text input screen; on ENVOI it returns to `return_screen` and calls `on_done`.
void request_text_input(screen_id_t return_screen,
                        const char* title,
                        const char* prompt,
                        const char* initial_value,
                        bool masked,
                        text_input_done_t on_done);
// Loads a document (local id, "/notes/idee.md") and opens the editor once it arrived, with
// the cursor on `line` (1-based, 0 = top, past the end = end); leaving the editor returns
// to `return_screen`.
void open_document_from(const char* id, uint16_t line, screen_id_t return_screen);
// millis() of the last key press.
uint32_t last_input_ms();

app_settings_t& settings();
SettingsService& settings_service();
ClockService& clock_service();
WifiService& wifi();
OtaService& ota();

}  // namespace shell

#endif
