#include "lock_screen.h"

#include <Arduino.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "app/app_shell.h"
#include "services/settings_service.h"
#include "ui/minitel_ui.h"

namespace {
constexpr const char* KEY_PIN = "pin";
constexpr size_t PIN_MIN_LENGTH = 4;
constexpr size_t PIN_MAX_LENGTH = 8;
constexpr uint8_t ATTEMPTS_BEFORE_DELAY = 3;
constexpr uint32_t LOCKOUT_MS = 30000;

struct lock_state_t {
  char entered[PIN_MAX_LENGTH + 1];
  uint8_t failures;
  uint32_t locked_until_ms;
};

lock_state_t g_lock;

void load_pin(char* out, size_t out_size) {
  shell::settings_service().load_secret_value(KEY_PIN, out, out_size);
}

bool is_valid_pin(const char* pin) {
  const size_t length = strlen(pin);
  if (length < PIN_MIN_LENGTH || length > PIN_MAX_LENGTH) {
    return false;
  }
  for (size_t i = 0; i < length; ++i) {
    if (!isdigit(static_cast<unsigned char>(pin[i]))) {
      return false;
    }
  }
  return true;
}

void check_entered_pin() {
  char pin[PIN_MAX_LENGTH + 1];
  load_pin(pin, sizeof(pin));
  if (strcmp(pin, g_lock.entered) == 0) {
    memset(&g_lock, 0, sizeof(g_lock));
    shell::switch_screen(SCREEN_HOME);
    return;
  }

  g_lock.entered[0] = '\0';
  ++g_lock.failures;
  if (g_lock.failures >= ATTEMPTS_BEFORE_DELAY) {
    g_lock.failures = 0;
    g_lock.locked_until_ms = millis() + LOCKOUT_MS;
    shell::set_status("Code incorrect: attendre 30 s");
  } else {
    shell::set_status("Code incorrect");
  }
}

}  // namespace

bool lock_screen_required() {
  char pin[PIN_MAX_LENGTH + 1];
  load_pin(pin, sizeof(pin));
  return pin[0] != '\0';
}

void lock_screen_open() {
  memset(&g_lock, 0, sizeof(g_lock));
  shell::switch_screen(SCREEN_LOCK);
}

void lock_screen_render() {
  shell::render_frame("VERROU", "Chiffres puis ENVOI  CORRECTION efface");
  minitel_ui_write_line(8, "      Minitel verrouille");
  char stars[PIN_MAX_LENGTH * 2 + 1];
  size_t length = 0;
  for (size_t i = 0; g_lock.entered[i] != '\0'; ++i) {
    stars[length++] = '*';
    stars[length++] = ' ';
  }
  stars[length] = '\0';
  char line[UI_COLS + 1];
  snprintf(line, sizeof(line), "      Code PIN: %s", stars);
  minitel_ui_write_line(11, line);
  shell::render_status_line();
}

void lock_screen_handle(const input_event_t& event) {
  if (g_lock.locked_until_ms != 0 && static_cast<int32_t>(millis() - g_lock.locked_until_ms) < 0) {
    return;  // waiting after repeated failures
  }
  g_lock.locked_until_ms = 0;

  const size_t length = strlen(g_lock.entered);
  if (event.type == INPUT_EVENT_PRINTABLE && isdigit(static_cast<unsigned char>(event.ch)) &&
      length < PIN_MAX_LENGTH) {
    g_lock.entered[length] = event.ch;
    g_lock.entered[length + 1] = '\0';
  } else if (event.type == INPUT_EVENT_BACKSPACE && length > 0) {
    g_lock.entered[length - 1] = '\0';
  } else if (event.type == INPUT_EVENT_ENTER) {
    check_entered_pin();
  } else {
    return;
  }
  shell::request_redraw();
}

void lock_screen_change_pin(const char* pin) {
  if (!pin || pin[0] == '\0') {
    shell::settings_service().store_secret_value(KEY_PIN, "");
    shell::set_status("Code PIN retire");
    return;
  }
  if (!is_valid_pin(pin)) {
    shell::set_status("PIN: 4 a 8 chiffres");
    return;
  }
  shell::settings_service().store_secret_value(KEY_PIN, pin);
  shell::set_status("Code PIN active au demarrage");
}
