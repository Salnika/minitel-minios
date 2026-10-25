#include "lib_minitel/ssh/ssh_client.h"

bool lm_ssh_init(void){ return true; }
bool lm_ssh_connect(lm_ssh_t* cli, const char* host, uint16_t port, const char* user, const char* pass){ (void)cli;(void)host;(void)port;(void)user;(void)pass; return false; }
int  lm_ssh_read(lm_ssh_t* cli, uint8_t* buf, uint16_t maxlen){ (void)cli;(void)buf;(void)maxlen; return 0; }
int  lm_ssh_write(lm_ssh_t* cli, const uint8_t* buf, uint16_t len){ (void)cli;(void)buf;(void)len; return 0; }
void lm_ssh_disconnect(lm_ssh_t* cli){ (void)cli; }
