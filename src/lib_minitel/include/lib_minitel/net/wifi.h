#ifndef LIB_MINITEL_WIFI_H
#define LIB_MINITEL_WIFI_H
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char ssid[33];
    int32_t rssi;
    bool encrypted;
} lm_wifi_network_t;

void lm_wifi_init(void);
bool lm_wifi_is_connected(void);
int  lm_wifi_scan(lm_wifi_network_t* out, int max_items);
bool lm_wifi_connect(const char* ssid, const char* password);
/* Like lm_wifi_connect, but stops waiting as soon as `cancelled` (may be NULL) returns true;
   the radio then goes on trying. */
bool lm_wifi_connect_cancellable(const char* ssid, const char* password, bool (*cancelled)(void));
void lm_wifi_disconnect(void);
void lm_wifi_get_ip(char* buf, size_t buflen);
void lm_wifi_get_ssid(char* buf, size_t buflen);

#ifdef __cplusplus
}
#endif
#endif
