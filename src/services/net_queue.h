#ifndef SERVICES_NET_QUEUE_H
#define SERVICES_NET_QUEUE_H

#include <stdint.h>

// Bookkeeping of the network requests. Only the loop task uses it: the network task just
// runs the slot it is handed (see net_worker.h). One request is in flight at a time.
//
// Foreground requests are the user's: one at a time, shown on the status line, cancelled
// by ANNULATION. Background requests (silent refreshes) run when no foreground request
// waits; asking again for one that is still queued merges with it.
namespace net_queue {

constexpr uint8_t NET_QUEUE_SLOTS = 4;
constexpr int8_t NET_QUEUE_NONE = -1;

enum slot_state_t : uint8_t {
  SLOT_FREE = 0,
  SLOT_QUEUED,     // waiting for the network task
  SLOT_RUNNING,    // on the network task
  SLOT_CANCELLED,  // still on the network task, its result will be dropped
  SLOT_DONE,       // finished, its result is being delivered on the loop
};

enum priority_t : uint8_t {
  PRIORITY_BACKGROUND = 0,
  PRIORITY_FOREGROUND,
};

enum cancel_result_t : uint8_t {
  CANCEL_NONE = 0,  // no foreground request was pending
  CANCEL_DROPPED,   // it was still queued: gone
  CANCEL_STOPPING,  // it runs: the network task stops it as soon as it can
};

struct slot_t {
  slot_state_t state;
  priority_t priority;
  uint8_t kind;
  uint32_t id;  // submission order, 0 for a free slot
};

struct queue_t {
  slot_t slots[NET_QUEUE_SLOTS];
  uint32_t next_id;
  int8_t in_flight;  // slot handed to the network task, NET_QUEUE_NONE when idle
};

void init(queue_t* queue);
// Takes a slot for a new request and returns it, or NET_QUEUE_NONE when it is refused: a
// foreground request while another one is pending, or no free slot. A background request
// whose kind is already queued returns that slot instead.
int8_t submit(queue_t* queue, uint8_t kind, priority_t priority);
// Slot to hand to the network task now: none while one is in flight, else the oldest
// foreground request, else the oldest background one.
int8_t next(const queue_t* queue);
void start(queue_t* queue, int8_t slot);
// The network task finished `slot`. Returns true when its result is to be delivered (the
// slot stays SLOT_DONE until release()); a cancelled slot is freed at once.
bool finish(queue_t* queue, int8_t slot);
void release(queue_t* queue, int8_t slot);
cancel_result_t cancel_foreground(queue_t* queue);
// Pending foreground request (queued or running, not cancelled), or NET_QUEUE_NONE.
int8_t foreground(const queue_t* queue);
// A background request of `kind` is queued or running.
bool background_pending(const queue_t* queue, uint8_t kind);
bool idle(const queue_t* queue);

}  // namespace net_queue

#endif
