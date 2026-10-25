#ifndef LIB_MINITEL_SSH_CLIENT_H
#define LIB_MINITEL_SSH_CLIENT_H
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct { void* _impl; } lm_ssh_t;

bool lm_ssh_init(void);
bool lm_ssh_connect(lm_ssh_t* cli, const char* host, uint16_t port,
                    const char* user, const char* pass);
int  lm_ssh_read(lm_ssh_t* cli, uint8_t* buf, uint16_t maxlen);
int  lm_ssh_write(lm_ssh_t* cli, const uint8_t* buf, uint16_t len);
void lm_ssh_disconnect(lm_ssh_t* cli);

#ifdef __cplusplus
}
#endif
#endif
