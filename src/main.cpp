#include <Arduino.h>

#include "app/app.h"

// HTTPS requests (and their TLS handshakes) now run on the network task, which has its own
// 16 KB stack (services/net_worker.h). The loop keeps 16 KB until Systeme > "Pile libre"
// shows on the device how much of it is used; 8 KB would give that much heap back.
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

void setup() {
  app_setup();
}

void loop() {
  app_loop();
}
