#include "minitel_link.h"

#include <stddef.h>

#include "platform/arduino_wrapper.h"

namespace {
constexpr uint8_t ESC = 0x1B;
constexpr uint8_t PRO1 = 0x39;
constexpr uint8_t PRO2 = 0x3A;
constexpr uint8_t PRO3 = 0x3B;
constexpr uint8_t PROG = 0x6B;
constexpr uint8_t STATUS_SPEED = 0x74;
constexpr uint8_t REP_STATUS_SPEED = 0x75;
constexpr uint8_t ROUTE_OFF = 0x60;
constexpr uint8_t ROUTE_ON = 0x61;
constexpr uint8_t ROUTE_STATUS = 0x63;

// Module codes: receiving side, then emitting side.
constexpr uint8_t RX_SCREEN = 0x58;
constexpr uint8_t RX_MODEM = 0x5A;
constexpr uint8_t RX_SOCKET = 0x5B;
constexpr uint8_t TX_SCREEN = 0x50;
constexpr uint8_t TX_KEYBOARD = 0x51;
constexpr uint8_t TX_MODEM = 0x52;
constexpr uint8_t TX_SOCKET = 0x53;

// SEP codes the Minitel sends on its own: connection, line and mode changes, reset.
constexpr uint8_t FIRST_STATUS_CODE = 0x50;
constexpr uint8_t LAST_STATUS_CODE = 0x5F;

constexpr uint32_t POWER_ON_BAUD = 1200;
constexpr uint32_t REPLY_TIMEOUT_MS = 250;
// After PROG the Minitel switches, then acknowledges at the new speed.
constexpr uint32_t SPEED_CHANGE_TIMEOUT_MS = 500;
// The codes of one connection attempt come within a second or so.
constexpr uint32_t STATUS_SETTLE_MS = 500;
// Bytes at another speed keep raising errors: one check per interval at most.
constexpr uint32_t ERROR_CHECK_INTERVAL_MS = 2000;

struct route_t {
  uint8_t command;
  uint8_t receiver;
  uint8_t emitter;
};

// The links that replace the standard ones come first, and nothing is cut unless they are
// confirmed: the page and the keyboard never go dead.
constexpr route_t ROUTES[] = {
    {ROUTE_ON, RX_SCREEN, TX_SOCKET},
    {ROUTE_ON, RX_SOCKET, TX_KEYBOARD},
    {ROUTE_OFF, RX_MODEM, TX_KEYBOARD},  // the local echo of typed keys
    {ROUTE_OFF, RX_MODEM, TX_SOCKET},
    {ROUTE_OFF, RX_SCREEN, TX_MODEM},
    {ROUTE_OFF, RX_SOCKET, TX_MODEM},
};

minitel_link_state_t g_link = {false, POWER_ON_BAUD, false, true, 0, 0, 0};
bool g_status_pending = false;
uint32_t g_status_due_ms = 0;
uint32_t g_last_check_ms = 0;

enum reply_kind_t {
  REPLY_NONE,
  REPLY_SPEED,  // ESC : u <speed>
  REPLY_ROUTE,  // ESC ; c <module> <links>
};

// Picks the Minitel replies out of the input, and notes when our speed query comes back.
struct reply_reader_t {
  uint8_t window[5];
  uint8_t length;
  bool query_echoed;
};

reply_kind_t read_reply(reply_reader_t* reader, uint8_t byte, uint8_t* module, uint8_t* value) {
  if (reader->length == sizeof(reader->window)) {
    for (uint8_t i = 1; i < sizeof(reader->window); ++i) {
      reader->window[i - 1] = reader->window[i];
    }
    --reader->length;
  }
  reader->window[reader->length++] = byte;
  const uint8_t* end = reader->window + reader->length;
  if (reader->length >= 3 && end[-3] == ESC && end[-2] == PRO1 && end[-1] == STATUS_SPEED) {
    reader->query_echoed = true;
  }
  if (reader->length >= 4 && end[-4] == ESC && end[-3] == PRO2 && end[-2] == REP_STATUS_SPEED) {
    *value = end[-1];
    reader->length = 0;
    return REPLY_SPEED;
  }
  if (reader->length == 5 && end[-5] == ESC && end[-4] == PRO3 && end[-3] == ROUTE_STATUS) {
    *module = end[-2];
    *value = end[-1];
    reader->length = 0;
    return REPLY_ROUTE;
  }
  return REPLY_NONE;
}

// Waits for a reply of `kind` (from `module` for a routing status). Other bytes are dropped.
bool wait_reply(reply_reader_t* reader,
                reply_kind_t kind,
                uint8_t module,
                uint32_t timeout_ms,
                uint8_t* value) {
  const uint32_t start_ms = get_millis();
  while (true) {
    while (uart_minitel_available()) {
      uint8_t reply_module = 0;
      uint8_t reply_value = 0;
      const reply_kind_t reply = read_reply(reader, uart_minitel_read(), &reply_module, &reply_value);
      if (reply == kind && (kind != REPLY_ROUTE || reply_module == module)) {
        *value = reply_value;
        return true;
      }
    }
    if (get_millis() - start_ms >= timeout_ms) {
      return false;
    }
    delay_ms(1);
  }
}

void send_bytes(const uint8_t* bytes, size_t length) {
  for (size_t i = 0; i < length; ++i) {
    uart_minitel_write(bytes[i]);
  }
}

// Speed byte of PROG and of its acknowledgment: 1, emission speed, reception speed.
uint8_t speed_code(uint32_t baud) {
  switch (baud) {
    case 300: return 0x52;
    case 1200: return 0x64;
    case 4800: return 0x76;
    case 9600: return 0x7F;  // Minitel 2 only
    default: return 0;
  }
}

uint32_t speed_from_code(uint8_t code) {
  switch (code) {
    case 0x52: return 300;
    case 0x64: return 1200;
    case 0x76: return 4800;
    case 0x7F: return 9600;
    default: return 0;
  }
}

// An answer means the Minitel and the UART agree on the speed.
bool query_speed(reply_reader_t* reader) {
  const uint8_t query[] = {ESC, PRO1, STATUS_SPEED};
  send_bytes(query, sizeof(query));
  uint8_t code = 0;
  return wait_reply(reader, REPLY_SPEED, 0, REPLY_TIMEOUT_MS, &code) && speed_from_code(code) != 0;
}

// The first of `candidates` the Minitel answers at (the UART stays there), else 0.
uint32_t find_speed(const uint32_t* candidates, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    bool tried = false;
    for (size_t j = 0; j < i; ++j) {
      tried = tried || candidates[j] == candidates[i];
    }
    if (tried) {
      continue;
    }
    uart_minitel_set_baud(candidates[i]);
    reply_reader_t reader = {};
    if (query_speed(&reader)) {
      return candidates[i];
    }
  }
  return 0;
}

bool change_speed(uint32_t baud) {
  const uint8_t command[] = {ESC, PRO2, PROG, speed_code(baud)};
  send_bytes(command, sizeof(command));
  uart_minitel_set_baud(baud);
  reply_reader_t reader = {};
  uint8_t code = 0;
  return wait_reply(&reader, REPLY_SPEED, 0, SPEED_CHANGE_TIMEOUT_MS, &code) &&
         speed_from_code(code) == baud;
}

// True when the acknowledgment shows the link in the requested state.
bool apply_route(minitel_link_state_t* link, const route_t& route) {
  const uint8_t command[] = {ESC, PRO3, route.command, route.receiver, route.emitter};
  send_bytes(command, sizeof(command));
  reply_reader_t reader = {};
  uint8_t links = 0;
  if (!wait_reply(&reader, REPLY_ROUTE, route.receiver, REPLY_TIMEOUT_MS, &links)) {
    return false;
  }
  switch (route.receiver) {
    case RX_SCREEN: link->screen_links = links; break;
    case RX_SOCKET: link->socket_links = links; break;
    case RX_MODEM: link->modem_links = links; break;
    default: break;
  }
  const bool linked = (links & (1U << (route.emitter - TX_SCREEN))) != 0;
  return linked == (route.command == ROUTE_ON);
}

void route_modules(minitel_link_state_t* link) {
  link->routed = true;
  for (const route_t& route : ROUTES) {
    if (!apply_route(link, route)) {
      link->routed = false;
      if (route.command == ROUTE_ON) {
        return;  // no replacement: cut nothing
      }
    }
  }
}

// Once routed, our bytes no longer come back: the filter stays on if they still do.
bool echo_gone() {
  reply_reader_t reader = {};
  return query_speed(&reader) && !reader.query_echoed;
}

void keep_link(const minitel_link_state_t& link) {
  uart_minitel_discard_input();
  uart_minitel_set_echo_filter(link.echo_filter);
  g_link = link;
  g_status_pending = false;
  g_last_check_ms = get_millis();
}

void set_up(uint32_t baud, const uint32_t* candidates, size_t count) {
  minitel_link_state_t link = {false, POWER_ON_BAUD, false, true, 0, 0, 0};
  uart_minitel_set_echo_filter(false);  // raw bytes while talking Protocole
  uint32_t found = find_speed(candidates, count);
  if (found != 0 && found != baud && speed_code(baud) != 0) {
    if (change_speed(baud)) {
      found = baud;
    } else {
      // Refused, or its acknowledgment got lost: find where the Minitel stands.
      const uint32_t retry[] = {baud, found};
      found = find_speed(retry, 2);
    }
  }
  link.online = found != 0;
  if (link.online) {
    link.baud = found;
    route_modules(&link);
    link.echo_filter = !(link.routed && echo_gone());
  } else {
    uart_minitel_set_baud(POWER_ON_BAUD);
  }
  keep_link(link);
}

// After status codes, the routing went back to standard but output waited: the page is
// intact even if the speed changed. After receive errors alone, either noise or a Minitel
// restarted at 1200 with a blank page.
minitel_link_event_t check_link(uint32_t baud, bool after_status) {
  if (g_link.online) {
    uart_minitel_set_echo_filter(false);
    reply_reader_t reader = {};
    if (query_speed(&reader)) {
      minitel_link_state_t link = g_link;
      if (after_status) {
        route_modules(&link);
        link.echo_filter = !(link.routed && echo_gone());
      }
      keep_link(link);
      return MINITEL_LINK_CHECKED;
    }
  }
  const bool page_intact = after_status && g_link.online;
  minitel_link_setup(baud);
  return g_link.online && !page_intact ? MINITEL_LINK_RESTARTED : MINITEL_LINK_CHECKED;
}
}  // namespace

const minitel_link_state_t& minitel_link_setup(uint32_t baud) {
  const uint32_t candidates[] = {g_link.online ? g_link.baud : POWER_ON_BAUD, POWER_ON_BAUD, baud};
  set_up(baud, candidates, sizeof(candidates) / sizeof(candidates[0]));
  return g_link;
}

const minitel_link_state_t& minitel_link_state(void) {
  return g_link;
}

void minitel_link_note_status(uint8_t code) {
  if (code < FIRST_STATUS_CODE || code > LAST_STATUS_CODE) {
    return;
  }
  g_status_pending = true;
  g_status_due_ms = get_millis() + STATUS_SETTLE_MS;
}

bool minitel_link_holding_output(void) {
  return g_status_pending;
}

minitel_link_event_t minitel_link_poll(uint32_t baud) {
  const uint32_t now = get_millis();
  const bool rx_error = uart_minitel_rx_error();
  if (g_status_pending) {
    if (static_cast<int32_t>(now - g_status_due_ms) >= 0) {
      return check_link(baud, true);
    }
    if (rx_error) {
      uart_minitel_discard_input();  // the codes may come at another speed
    }
    return MINITEL_LINK_IDLE;
  }
  if (!rx_error) {
    return MINITEL_LINK_IDLE;
  }
  // A Minitel switched on later talks at 1200 like us: its status codes bring it back.
  if (!g_link.online || now - g_last_check_ms < ERROR_CHECK_INTERVAL_MS) {
    uart_minitel_discard_input();
    return MINITEL_LINK_IDLE;
  }
  return check_link(baud, false);
}
