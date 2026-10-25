#ifndef SERVICES_TLS_ROOTS_H
#define SERVICES_TLS_ROOTS_H

#include <string.h>

// PEM bundle of the root CAs trusted for HTTPS (Bibliotek behind Coolify, AI inference).
extern const char TLS_ROOT_CERTS[];

// The core default is 120 s: a stalled server or captive portal would freeze the UI.
constexpr unsigned long TLS_HANDSHAKE_TIMEOUT_S = 10;

inline bool url_uses_tls(const char* url) {
  return url && strncmp(url, "https://", 8) == 0;
}

#endif
