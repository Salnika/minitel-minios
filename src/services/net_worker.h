#ifndef SERVICES_NET_WORKER_H
#define SERVICES_NET_WORKER_H

#include <stddef.h>
#include <stdint.h>

#include "net_queue.h"

// Runs network requests on a dedicated FreeRTOS task, one at a time, so that the loop keeps
// drawing and reading keys. The queue bookkeeping (net_queue) stays on the loop; requests
// are slot indexes whose parameters and results the caller keeps in its own storage.
//
// Ownership: the loop fills a slot's parameters before pump() hands it over; the network
// task uses them, and fills the results, until it finishes; the loop then reads the results
// in the finish callback. The FreeRTOS queues carrying the slot index order these accesses.
// While a request runs, the tasks share nothing else than the atomics behind
// cancel_requested() and report()/progress().
namespace net_worker {

// A TLS handshake alone takes several KB of stack.
constexpr uint32_t NET_TASK_STACK_BYTES = 16 * 1024;

// Loop, right before `slot` starts: copy the inputs it needs into the shared area.
typedef void (*prepare_fn_t)(int8_t slot);
// Network task: performs `slot`. It may block for seconds, but must never touch the UART.
typedef void (*run_fn_t)(int8_t slot);
// Loop: `slot` finished and was not cancelled.
typedef void (*finish_fn_t)(int8_t slot);

// Request-specific progress of the request in flight.
struct progress_t {
  uint32_t stage;
  uint32_t amount;      // bytes ready, percent...
  const char* message;  // string literal or null
};

bool begin(prepare_fn_t prepare, run_fn_t run, finish_fn_t finish);
bool started();

// Loop side.
// Reserves a slot (see net_queue::submit): fill its parameters, then call pump().
int8_t submit(uint8_t kind, net_queue::priority_t priority);
// Hands the next request to the network task when it is free. Deferred while finish
// callbacks run: the shared results must not be overwritten under them.
void pump();
// Delivers the finished request, then starts the next one. Call it every loop iteration.
void poll();
net_queue::cancel_result_t cancel_foreground();
int8_t foreground();
int8_t in_flight();
const net_queue::slot_t& slot_info(int8_t slot);
bool background_pending(uint8_t kind);
progress_t progress();

// Network task side.
bool cancel_requested();
void report(uint32_t stage, uint32_t amount, const char* message);

// Lowest free stack seen on the network task, in bytes.
uint32_t stack_free_bytes();

}  // namespace net_worker

#endif
