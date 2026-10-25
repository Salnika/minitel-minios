#include "net_worker.h"

#include <Arduino.h>
#include <atomic>

#include "platform/app_log.h"

namespace net_worker {
namespace {

// Same core and priority as the Arduino loop, which time-slices with it, as the HTTP code
// ran there before. Core 0's idle task is watched by the task watchdog: a long download
// at priority 1 there could starve it and reset the board.
constexpr UBaseType_t NET_TASK_PRIORITY = 1;
constexpr BaseType_t NET_TASK_CORE = ARDUINO_RUNNING_CORE;

net_queue::queue_t g_queue;
QueueHandle_t g_start_queue = nullptr;
QueueHandle_t g_finish_queue = nullptr;
TaskHandle_t g_task = nullptr;
prepare_fn_t g_prepare = nullptr;
run_fn_t g_run = nullptr;
finish_fn_t g_finish = nullptr;
bool g_delivering = false;
const net_queue::slot_t g_no_slot = {};

std::atomic<bool> g_cancel(false);
std::atomic<uint32_t> g_stage(0);
std::atomic<uint32_t> g_amount(0);
std::atomic<const char*> g_message(nullptr);

void task_main(void*) {
  for (;;) {
    int8_t slot = net_queue::NET_QUEUE_NONE;
    if (xQueueReceive(g_start_queue, &slot, portMAX_DELAY) != pdTRUE) {
      continue;
    }
    g_run(slot);
    xQueueSend(g_finish_queue, &slot, portMAX_DELAY);
  }
}

}  // namespace

bool begin(prepare_fn_t prepare, run_fn_t run, finish_fn_t finish) {
  if (g_task) {
    return true;
  }
  net_queue::init(&g_queue);
  g_prepare = prepare;
  g_run = run;
  g_finish = finish;
  // One request in flight: a single place each way is enough.
  g_start_queue = xQueueCreate(1, sizeof(int8_t));
  g_finish_queue = xQueueCreate(1, sizeof(int8_t));
  if (!g_start_queue || !g_finish_queue || !run) {
    app_log_error("net", "queues not created");
    return false;
  }
  if (xTaskCreatePinnedToCore(task_main, "net", NET_TASK_STACK_BYTES, nullptr, NET_TASK_PRIORITY, &g_task,
                              NET_TASK_CORE) != pdPASS) {
    g_task = nullptr;
    app_log_error("net", "task not created (heap %lu)", static_cast<unsigned long>(ESP.getFreeHeap()));
    return false;
  }
  app_log_info("net", "task started stack=%lu heap=%lu", static_cast<unsigned long>(NET_TASK_STACK_BYTES),
               static_cast<unsigned long>(ESP.getFreeHeap()));
  return true;
}

bool started() {
  return g_task != nullptr;
}

int8_t submit(uint8_t kind, net_queue::priority_t priority) {
  if (!g_task) {
    return net_queue::NET_QUEUE_NONE;
  }
  return net_queue::submit(&g_queue, kind, priority);
}

void pump() {
  if (!g_task || g_delivering) {
    return;
  }
  const int8_t slot = net_queue::next(&g_queue);
  if (slot == net_queue::NET_QUEUE_NONE) {
    return;
  }
  g_cancel.store(false);
  g_stage.store(0);
  g_amount.store(0);
  g_message.store(nullptr);
  if (g_prepare) {
    g_prepare(slot);
  }
  net_queue::start(&g_queue, slot);
  // The task already took the previous slot out of the queue before finishing it.
  if (xQueueSend(g_start_queue, &slot, 0) != pdTRUE) {
    app_log_error("net", "start queue full, slot=%d", slot);
  }
}

void poll() {
  if (!g_task) {
    return;
  }
  int8_t slot = net_queue::NET_QUEUE_NONE;
  while (xQueueReceive(g_finish_queue, &slot, 0) == pdTRUE) {
    if (!net_queue::finish(&g_queue, slot)) {
      continue;  // cancelled: nobody waits for it
    }
    g_delivering = true;
    if (g_finish) {
      g_finish(slot);
    }
    g_delivering = false;
    net_queue::release(&g_queue, slot);
  }
  pump();
}

net_queue::cancel_result_t cancel_foreground() {
  const net_queue::cancel_result_t result = net_queue::cancel_foreground(&g_queue);
  if (result == net_queue::CANCEL_STOPPING) {
    g_cancel.store(true);  // only the request in flight can be the one stopping
  }
  return result;
}

int8_t foreground() {
  return net_queue::foreground(&g_queue);
}

int8_t in_flight() {
  return g_queue.in_flight;
}

const net_queue::slot_t& slot_info(int8_t slot) {
  if (slot < 0 || slot >= static_cast<int8_t>(net_queue::NET_QUEUE_SLOTS)) {
    return g_no_slot;
  }
  return g_queue.slots[slot];
}

bool background_pending(uint8_t kind) {
  return net_queue::background_pending(&g_queue, kind);
}

progress_t progress() {
  progress_t value;
  value.stage = g_stage.load();
  value.amount = g_amount.load();
  value.message = g_message.load();
  return value;
}

bool cancel_requested() {
  return g_cancel.load();
}

void report(uint32_t stage, uint32_t amount, const char* message) {
  g_stage.store(stage);
  g_message.store(message);
  // Last: the loop reads text up to `amount` once it sees it.
  g_amount.store(amount);
}

uint32_t stack_free_bytes() {
  return g_task ? static_cast<uint32_t>(uxTaskGetStackHighWaterMark(g_task)) : 0;
}

}  // namespace net_worker
