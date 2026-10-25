#ifndef APP_WORKSPACE_H
#define APP_WORKSPACE_H

#include <stddef.h>
#include <stdint.h>

#include "storage/document_store.h"
#include "storage/workspace_utils.h"

// Background side of the project manager: today's tasks (dashboard and reminders), the
// project timer and the recently opened documents.
namespace workspace {

constexpr uint8_t RECENT_MAX = 4;

struct recent_document_t {
  char id[STORE_MAX_DOCUMENT_ID + 1];  // local id, "/notes/idee.md"
  char title[STORE_MAX_TITLE_LENGTH + STORE_MAX_EXTENSION_LENGTH + 2];
};

// Receives the outcome of timer_stop(): `stopped` false means nothing was logged and the
// timer still runs; `status` is only valid during the call.
typedef void (*timer_stopped_t)(bool stopped, const char* status, uint32_t minutes);

void begin();
// From the main loop. Reloads today's tasks in the background every few minutes and rings
// the bell for timed tasks.
void tick();
// Reload today's tasks soon (a task was added or checked).
void request_refresh();
bool today_known();
uint16_t today_open_count();  // due today or late
uint16_t today_overdue_count();

// One project timer at a time, kept across reboots.
bool timer_running();
const char* timer_project();
uint32_t timer_elapsed_minutes();
bool timer_start(const char* project);
// Logs "- YYYY-MM-DD HH:MM +N min" in projets/<project>/temps.md, then stops the timer and
// calls `done` (right away under a minute). `label` shows on the status line meanwhile.
// False when the request could not be sent.
bool timer_stop(const char* label, timer_stopped_t done);

void recent_add(const char* id, const char* title);
uint8_t recent_count();
const recent_document_t& recent_at(uint8_t index);

}  // namespace workspace

#endif
