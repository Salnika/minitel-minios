#ifndef APP_SCREENS_MODEL_PICKER_H
#define APP_SCREENS_MODEL_PICKER_H

#include "app/input.h"

// Lists the DigitalOcean chat models and saves the chosen one as the AI model. Falls back
// to typing the model id when the list cannot be loaded.
void model_picker_open();
void model_picker_render();
void model_picker_handle(const input_event_t& event);

#endif
