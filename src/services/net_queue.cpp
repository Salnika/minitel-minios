#include "net_queue.h"

#include <string.h>

namespace net_queue {
namespace {

bool is_valid(const queue_t* queue, int8_t slot) {
  return queue && slot >= 0 && slot < static_cast<int8_t>(NET_QUEUE_SLOTS);
}

bool is_pending(const slot_t& slot) {
  return slot.state == SLOT_QUEUED || slot.state == SLOT_RUNNING;
}

void free_slot(slot_t* slot) {
  memset(slot, 0, sizeof(*slot));
}

}  // namespace

void init(queue_t* queue) {
  if (!queue) {
    return;
  }
  memset(queue, 0, sizeof(*queue));
  queue->next_id = 1;
  queue->in_flight = NET_QUEUE_NONE;
}

int8_t submit(queue_t* queue, uint8_t kind, priority_t priority) {
  if (!queue) {
    return NET_QUEUE_NONE;
  }
  if (priority == PRIORITY_FOREGROUND) {
    if (foreground(queue) != NET_QUEUE_NONE) {
      return NET_QUEUE_NONE;
    }
  } else {
    for (uint8_t i = 0; i < NET_QUEUE_SLOTS; ++i) {
      const slot_t& slot = queue->slots[i];
      if (slot.state == SLOT_QUEUED && slot.priority == PRIORITY_BACKGROUND && slot.kind == kind) {
        return static_cast<int8_t>(i);
      }
    }
  }

  for (uint8_t i = 0; i < NET_QUEUE_SLOTS; ++i) {
    slot_t& slot = queue->slots[i];
    if (slot.state == SLOT_FREE) {
      slot.state = SLOT_QUEUED;
      slot.priority = priority;
      slot.kind = kind;
      slot.id = queue->next_id++;
      return static_cast<int8_t>(i);
    }
  }
  return NET_QUEUE_NONE;
}

int8_t next(const queue_t* queue) {
  if (!queue || queue->in_flight != NET_QUEUE_NONE) {
    return NET_QUEUE_NONE;
  }
  int8_t best = NET_QUEUE_NONE;
  for (uint8_t i = 0; i < NET_QUEUE_SLOTS; ++i) {
    const slot_t& slot = queue->slots[i];
    if (slot.state != SLOT_QUEUED) {
      continue;
    }
    if (best == NET_QUEUE_NONE) {
      best = static_cast<int8_t>(i);
      continue;
    }
    const slot_t& current = queue->slots[best];
    if (slot.priority > current.priority || (slot.priority == current.priority && slot.id < current.id)) {
      best = static_cast<int8_t>(i);
    }
  }
  return best;
}

void start(queue_t* queue, int8_t slot) {
  if (!is_valid(queue, slot) || queue->in_flight != NET_QUEUE_NONE ||
      queue->slots[slot].state != SLOT_QUEUED) {
    return;
  }
  queue->slots[slot].state = SLOT_RUNNING;
  queue->in_flight = slot;
}

bool finish(queue_t* queue, int8_t slot) {
  if (!is_valid(queue, slot) || queue->in_flight != slot) {
    return false;
  }
  queue->in_flight = NET_QUEUE_NONE;
  if (queue->slots[slot].state == SLOT_RUNNING) {
    queue->slots[slot].state = SLOT_DONE;
    return true;
  }
  free_slot(&queue->slots[slot]);  // cancelled meanwhile: nobody waits for it
  return false;
}

void release(queue_t* queue, int8_t slot) {
  if (is_valid(queue, slot) && queue->slots[slot].state == SLOT_DONE) {
    free_slot(&queue->slots[slot]);
  }
}

cancel_result_t cancel_foreground(queue_t* queue) {
  const int8_t slot = foreground(queue);
  if (slot == NET_QUEUE_NONE) {
    return CANCEL_NONE;
  }
  if (queue->slots[slot].state == SLOT_QUEUED) {
    free_slot(&queue->slots[slot]);
    return CANCEL_DROPPED;
  }
  queue->slots[slot].state = SLOT_CANCELLED;
  return CANCEL_STOPPING;
}

int8_t foreground(const queue_t* queue) {
  if (!queue) {
    return NET_QUEUE_NONE;
  }
  for (uint8_t i = 0; i < NET_QUEUE_SLOTS; ++i) {
    const slot_t& slot = queue->slots[i];
    if (slot.priority == PRIORITY_FOREGROUND && is_pending(slot)) {
      return static_cast<int8_t>(i);
    }
  }
  return NET_QUEUE_NONE;
}

bool background_pending(const queue_t* queue, uint8_t kind) {
  if (!queue) {
    return false;
  }
  for (uint8_t i = 0; i < NET_QUEUE_SLOTS; ++i) {
    const slot_t& slot = queue->slots[i];
    if (slot.priority == PRIORITY_BACKGROUND && slot.kind == kind && is_pending(slot)) {
      return true;
    }
  }
  return false;
}

bool idle(const queue_t* queue) {
  if (!queue) {
    return true;
  }
  for (uint8_t i = 0; i < NET_QUEUE_SLOTS; ++i) {
    if (queue->slots[i].state != SLOT_FREE) {
      return false;
    }
  }
  return true;
}

}  // namespace net_queue
