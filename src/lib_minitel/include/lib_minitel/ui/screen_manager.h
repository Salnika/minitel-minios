#ifndef LIB_MINITEL_SCREEN_MANAGER_H
#define LIB_MINITEL_SCREEN_MANAGER_H
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef enum { LM_SCREEN_NONE = 0, LM_SCREEN_USER0 = 1 } lm_screen_type_t;

typedef struct lm_screen_interface_s {
    lm_screen_type_t type;
    void (*init)(void);
    void (*update)(void);
    void (*render)(void);
    bool (*handle_input)(uint8_t ch);
    void (*cleanup)(void);
} lm_screen_interface_t;

void lm_screens_init(void);
void lm_screens_register(const lm_screen_interface_t* screen);
void lm_screens_switch(lm_screen_type_t type);
void lm_screens_update(void);
void lm_screens_render(void);
bool lm_screens_handle_input(uint8_t ch);

#ifdef __cplusplus
}
#endif
#endif
