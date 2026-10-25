#include "model_picker.h"

#include <stdio.h>
#include <string.h>

#include "app/app_shell.h"
#include "app/lang.h"
#include "services/settings_service.h"
#include "ui/minitel_ui.h"

namespace {
constexpr uint8_t MAX_MODELS = 40;
constexpr uint8_t LIST_START_ROW = 5;
constexpr uint8_t LIST_END_ROW = UI_STATUS_ROW - 2;
constexpr uint8_t VISIBLE_ROWS = LIST_END_ROW - LIST_START_ROW + 1;
constexpr uint8_t PAGE_STEP = 5;
static_assert(MAX_MODELS <= NET_MAX_MODELS, "net replies too small");

struct model_picker_state_t {
  char models[MAX_MODELS][CHAT_MAX_MODEL_ID_LENGTH + 1];
  uint8_t count;
  uint8_t selected;
  uint8_t scroll;
};

model_picker_state_t g_picker;

void ensure_selection_visible() {
  if (g_picker.selected < g_picker.scroll) {
    g_picker.scroll = g_picker.selected;
  } else if (g_picker.selected >= g_picker.scroll + VISIBLE_ROWS) {
    g_picker.scroll = static_cast<uint8_t>(g_picker.selected - VISIBLE_ROWS + 1);
  }
}

void apply_model(const char* model) {
  if (!model || model[0] == '\0') {
    shell::set_status(TR("Modele vide: inchange", "Empty model: unchanged"));
    return;
  }
  app_settings_t& settings = shell::settings();
  strncpy(settings.ai_model, model, sizeof(settings.ai_model) - 1);
  settings.ai_model[sizeof(settings.ai_model) - 1] = '\0';
  shell::settings_service().save(settings);  // the next chat request reads it

  char status[41];
  snprintf(status, sizeof(status), TR("Modele: %.32s", "Model: %.32s"), settings.ai_model);
  shell::set_status(status);
}

void open_manual_input() {
  shell::request_text_input(SCREEN_SETTINGS,
                            TR("Modele IA", "AI model"),
                            TR("Identifiant du modele", "Model id"),
                            shell::settings().ai_model,
                            false,
                            apply_model);
}

void on_models_loaded(const net_request_t& request, const net_reply_t& reply) {
  (void)request;
  const int count = reply.ok ? (reply.count < MAX_MODELS ? reply.count : MAX_MODELS) : 0;
  if (count <= 0) {
    // Offline, missing key...: the model can still be typed by hand.
    char reason[41];
    snprintf(reason, sizeof(reason), "%.40s", reply.ok ? TR("Aucun modele", "No model") : reply.error);
    open_manual_input();
    shell::set_status(reason);
    return;
  }

  memcpy(g_picker.models, reply.data.models, sizeof(g_picker.models[0]) * count);
  g_picker.count = static_cast<uint8_t>(count);
  g_picker.selected = 0;
  g_picker.scroll = 0;
  for (uint8_t i = 0; i < g_picker.count; ++i) {
    if (strcmp(g_picker.models[i], shell::settings().ai_model) == 0) {
      g_picker.selected = i;
      break;
    }
  }
  ensure_selection_visible();
  shell::switch_screen(SCREEN_MODEL_PICKER);
}

}  // namespace

void model_picker_open() {
  net_request_t request =
      net::make(NET_LIST_MODELS, TR("Chargement des modeles...", "Loading models..."), on_models_loaded);
  net::submit(request);
}

void model_picker_render() {
  shell::render_frame(TR("MODELE IA", "AI MODEL"),
                      TR("ENVOI choisir REP/SUITE RET M saisir", "ENVOI choose REP/SUITE RET M type"));
  minitel_ui_write_line(3, TR("Modeles DigitalOcean (* = actuel)", "DigitalOcean models (* = current)"));

  for (uint8_t row = 0; row < VISIBLE_ROWS; ++row) {
    const uint8_t index = static_cast<uint8_t>(g_picker.scroll + row);
    char line[UI_COLS + 1] = "";
    if (index < g_picker.count) {
      snprintf(line,
               sizeof(line),
               "%c %.36s%s",
               index == g_picker.selected ? '>' : ' ',
               g_picker.models[index],
               strcmp(g_picker.models[index], shell::settings().ai_model) == 0 ? " *" : "");
    }
    minitel_ui_write_line(static_cast<uint8_t>(LIST_START_ROW + row), line);
  }
  shell::render_status_line();
}

void model_picker_handle(const input_event_t& event) {
  switch (event.type) {
    case INPUT_EVENT_UP:
      if (g_picker.selected > 0) {
        --g_picker.selected;
      }
      break;
    case INPUT_EVENT_DOWN:
      if (g_picker.selected + 1 < g_picker.count) {
        ++g_picker.selected;
      }
      break;
    case INPUT_EVENT_PAGE_UP:
      g_picker.selected = g_picker.selected > PAGE_STEP ? static_cast<uint8_t>(g_picker.selected - PAGE_STEP) : 0;
      break;
    case INPUT_EVENT_PAGE_DOWN:
      g_picker.selected = static_cast<uint8_t>(g_picker.selected + PAGE_STEP < g_picker.count
                                                   ? g_picker.selected + PAGE_STEP
                                                   : g_picker.count - 1);
      break;
    case INPUT_EVENT_ENTER:
      shell::switch_screen(SCREEN_SETTINGS);
      apply_model(g_picker.models[g_picker.selected]);
      return;
    case INPUT_EVENT_ESCAPE:
    case INPUT_EVENT_CANCEL:
      shell::switch_screen(SCREEN_SETTINGS);
      return;
    case INPUT_EVENT_PRINTABLE:
      if (event.ch == 'm' || event.ch == 'M') {
        open_manual_input();
      }
      return;
    default:
      return;
  }
  ensure_selection_visible();
  shell::request_redraw();
}
