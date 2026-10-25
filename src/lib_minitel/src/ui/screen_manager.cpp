#include "lib_minitel/ui/screen_manager.h"

#define LM_MAX_SCREENS 8
static lm_screen_interface_t g_screens[LM_MAX_SCREENS];
static unsigned g_count = 0;
static lm_screen_type_t g_current = LM_SCREEN_NONE;
static lm_screen_interface_t* g_curr = nullptr;

extern "C" {
void lm_screens_init(void) { g_count = 0; g_current = LM_SCREEN_NONE; g_curr = nullptr; }
void lm_screens_register(const lm_screen_interface_t* s) { if (!s || g_count >= LM_MAX_SCREENS) return; g_screens[g_count++] = *s; }
void lm_screens_switch(lm_screen_type_t t) {
    if (g_curr && g_curr->cleanup) g_curr->cleanup();
    g_curr = nullptr;
    for (unsigned i = 0; i < g_count; i++) { if (g_screens[i].type == t) { g_curr = &g_screens[i]; break; } }
    g_current = t;
    if (g_curr && g_curr->init) g_curr->init();
    if (g_curr && g_curr->render) g_curr->render();
}
void lm_screens_update(void) { if (g_curr && g_curr->update) g_curr->update(); }
void lm_screens_render(void) { if (g_curr && g_curr->render) g_curr->render(); }
bool lm_screens_handle_input(uint8_t ch) { if (g_curr && g_curr->handle_input) return g_curr->handle_input(ch); return false; }
}
