#ifndef UI_MINITEL_LINK_H
#define UI_MINITEL_LINK_H

#include <stdint.h>

// The serial link with the Minitel, set up with Protocole commands (STUM 1B, chapter 6): the
// speed of the socket and the routing between the screen, keyboard, modem and socket.
struct minitel_link_state_t {
  bool online;  // the Minitel answers
  uint32_t baud;
  // The socket draws straight on the screen and the keyboard sends to the socket only: the
  // Minitel no longer prints typed keys itself (the modem loop of its local state), which
  // used to land in the middle of the page being drawn.
  bool routed;
  // Our own bytes come back on the input and are filtered out of it. Kept whenever the
  // routing could not be confirmed or the probe still sees them.
  bool echo_filter;
  // Routing status of the screen, socket and modem from the acknowledgments: one bit per
  // emitting module linked to them (screen 0x01, keyboard 0x02, modem 0x04, socket 0x08).
  uint8_t screen_links;
  uint8_t socket_links;
  uint8_t modem_links;
};

enum minitel_link_event_t {
  MINITEL_LINK_IDLE = 0,
  // Checked (and routed again if needed): what is on the page is still right.
  MINITEL_LINK_CHECKED,
  // Found again after it restarted or talked at another speed: the page is blank or holds
  // garbage, redraw everything.
  MINITEL_LINK_RESTARTED,
};

// Finds the Minitel (1200 baud after power-on, or `baud` when it kept our setting while we
// restarted), switches it to `baud` and routes its modules. Blocks a few hundred ms; when no
// Minitel answers, the link stays at 1200 baud with the echo filter.
const minitel_link_state_t& minitel_link_setup(uint32_t baud);
const minitel_link_state_t& minitel_link_state(void);

// The code after a SEP from the Minitel. 0x50-0x5F are its own status codes: connection
// attempts (CONNEXION/FIN) and line changes put its routing, maybe its speed, back to
// standard. The link is checked once the codes settle.
void minitel_link_note_status(uint8_t code);
// True while such a check is due: hold drawing so nothing is sent at the wrong speed.
bool minitel_link_holding_output(void);

// Called by the loop before reading keys: after status codes, or bytes received at another
// speed, checks the Minitel and sets the link up again.
minitel_link_event_t minitel_link_poll(uint32_t baud);

#endif
