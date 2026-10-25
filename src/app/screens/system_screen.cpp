#include "system_screen.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_system.h>
#include <stdio.h>
#include <string.h>

#include "app/app_shell.h"
#include "app/version.h"
#include "services/ota_service.h"
#include "services/ota_utils.h"
#include "services/settings_service.h"
#include "services/wifi_service.h"
#include "ui/minitel_link.h"
#include "ui/minitel_ui.h"

namespace {
constexpr uint8_t ACTIONS_ROW = 16;

enum system_action_t {
  SYSTEM_ACTION_UPDATE = 0,
  SYSTEM_ACTION_RESTART,
  SYSTEM_ACTION_COUNT
};

struct system_screen_state_t {
  uint8_t selected;
  // A first ENVOI arms the action (update found / restart asked), the second runs it.
  bool armed;
  char available_version[NET_MAX_VERSION];
};

system_screen_state_t g_system;

const char* reset_reason_label() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "mise sous tension";
    case ESP_RST_EXT: return "bouton reset";
    case ESP_RST_SW: return "redemarrage logiciel";
    case ESP_RST_PANIC: return "plantage";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_BROWNOUT: return "baisse de tension";
    case ESP_RST_DEEPSLEEP: return "reveil";
    default: return "inconnu";
  }
}

void format_uptime(char* out, size_t out_size) {
  const uint32_t total_minutes = millis() / 60000UL;
  const uint32_t days = total_minutes / (24UL * 60UL);
  const uint32_t hours = (total_minutes / 60UL) % 24UL;
  const uint32_t minutes = total_minutes % 60UL;
  if (days > 0) {
    snprintf(out, out_size, "%lu j %02lu:%02lu", static_cast<unsigned long>(days),
             static_cast<unsigned long>(hours), static_cast<unsigned long>(minutes));
  } else {
    snprintf(out, out_size, "%02lu:%02lu", static_cast<unsigned long>(hours),
             static_cast<unsigned long>(minutes));
  }
}

const char* action_label(uint8_t action) {
  switch (action) {
    case SYSTEM_ACTION_UPDATE:
      return g_system.armed && g_system.selected == SYSTEM_ACTION_UPDATE ? "Installer la mise a jour"
                                                                          : "Verifier la mise a jour";
    case SYSTEM_ACTION_RESTART:
      return "Redemarrer";
  }
  return "";
}

void on_update_checked(const net_request_t& request, const net_reply_t& reply) {
  (void)request;
  if (!reply.ok) {
    shell::set_status(reply.error);
    return;
  }
  char status[41];
  if (!reply.data.update.newer) {
    ota_utils::up_to_date_status(MINIOS_FIRMWARE_VERSION, reply.data.update.version, status,
                                 sizeof(status));
    shell::set_status(status);
    return;
  }
  strncpy(g_system.available_version, reply.data.update.version, sizeof(g_system.available_version) - 1);
  g_system.available_version[sizeof(g_system.available_version) - 1] = '\0';
  g_system.armed = true;
  snprintf(status, sizeof(status), "%s dispo: ENVOI pour installer", g_system.available_version);
  shell::set_status(status);
  shell::request_redraw();
}

// Only called when the installation failed: it reboots on success.
void on_update_installed(const net_request_t& request, const net_reply_t& reply) {
  (void)request;
  shell::set_status(reply.error[0] ? reply.error : "Mise a jour echouee");
}

void run_update_action() {
  if (!g_system.armed) {
    net_request_t request = net::make(NET_CHECK_UPDATE, "Recherche de mise a jour...", on_update_checked);
    net::submit(request);
    return;
  }

  g_system.armed = false;
  // The status line follows the download; ANNULATION stops it before anything is flashed.
  net_request_t request = net::make(NET_INSTALL_UPDATE, "Mise a jour, ne pas eteindre", on_update_installed);
  net::submit(request);
}

void run_restart_action() {
  if (!g_system.armed) {
    g_system.armed = true;
    shell::set_status("ENVOI encore pour redemarrer");
    return;
  }
  shell::show_busy_status("Redemarrage...");
  delay(300);
  ESP.restart();
}

}  // namespace

void system_screen_open() {
  g_system.selected = 0;
  g_system.armed = false;
  g_system.available_version[0] = '\0';
  shell::switch_screen(SCREEN_SYSTEM);
}

void system_screen_render() {
  shell::render_frame("SYSTEME", "ENVOI action REP/SUITE nav RETOUR");
  char line[UI_COLS + 1];

  snprintf(line, sizeof(line), "Firmware %s  %s r%u %luMHz", MINIOS_FIRMWARE_VERSION,
           ESP.getChipModel(), static_cast<unsigned>(ESP.getChipRevision()),
           static_cast<unsigned long>(ESP.getCpuFreqMHz()));
  minitel_ui_write_line(3, line);
  snprintf(line, sizeof(line), "Memoire: %lu Ko libres (min %lu)",
           static_cast<unsigned long>(ESP.getFreeHeap() / 1024),
           static_cast<unsigned long>(ESP.getMinFreeHeap() / 1024));
  minitel_ui_write_line(4, line);
  snprintf(line, sizeof(line), "Flash: %lu Ko / %lu Ko par slot",
           static_cast<unsigned long>(ESP.getSketchSize() / 1024),
           static_cast<unsigned long>((ESP.getSketchSize() + ESP.getFreeSketchSpace()) / 1024));
  minitel_ui_write_line(5, line);
  char uptime[16];
  format_uptime(uptime, sizeof(uptime));
  snprintf(line, sizeof(line), "En marche depuis %s", uptime);
  minitel_ui_write_line(6, line);
  snprintf(line, sizeof(line), "Demarrage: %s", reset_reason_label());
  minitel_ui_write_line(7, line);
  // Lowest free stack seen so far: how much the stack sizes could shrink.
  snprintf(line, sizeof(line), "Pile libre: loop %lu o, net %lu o",
           static_cast<unsigned long>(uxTaskGetStackHighWaterMark(nullptr)),
           static_cast<unsigned long>(net::stack_free_bytes()));
  minitel_ui_write_line(8, line);

  char ssid[SETTINGS_MAX_WIFI_SSID + 1];
  char ip[20];
  shell::wifi().get_ssid(ssid, sizeof(ssid));
  shell::wifi().get_ip(ip, sizeof(ip));
  if (shell::wifi().is_connected()) {
    snprintf(line, sizeof(line), "WiFi: %.26s (%ld dBm)", ssid, static_cast<long>(WiFi.RSSI()));
  } else {
    snprintf(line, sizeof(line), "WiFi: hors ligne");
  }
  minitel_ui_write_line(9, line);
  snprintf(line, sizeof(line), "IP: %s", ip);
  minitel_ui_write_line(10, line);
  // Pushed from a computer on the same Wi-Fi (pio run -e ota), unlike the server updates.
  if (shell::ota().lan_enabled()) {
    snprintf(line, sizeof(line), "Maj par PC: %.20s.local", shell::ota().hostname());
  } else {
    snprintf(line, sizeof(line), "Maj par PC: off (token API vide)");
  }
  minitel_ui_write_line(11, line);
  // The Docs URL serves the notes and the updates checked below.
  if (net::store_available()) {
    char host[32];
    ota_utils::url_host(shell::settings().api_url, host, sizeof(host));
    snprintf(line, sizeof(line), "Serveur ok: %.29s", host);
  } else {
    snprintf(line, sizeof(line), "Serveur: %.31s", net::store_error());
  }
  minitel_ui_write_line(12, line);
  snprintf(line, sizeof(line), "IA: %.36s", shell::settings().ai_model);
  minitel_ui_write_line(13, line);
  const minitel_link_state_t& link = minitel_link_state();
  if (link.online) {
    snprintf(line, sizeof(line), "Minitel: %lu bauds, echo %s%s",
             static_cast<unsigned long>(link.baud),
             link.routed ? "coupe" : "local",
             link.echo_filter ? ", filtre" : "");
  } else {
    snprintf(line, sizeof(line), "Minitel: pas de reponse (%lu bauds)",
             static_cast<unsigned long>(link.baud));
  }
  minitel_ui_write_line(14, line);

  for (uint8_t action = 0; action < SYSTEM_ACTION_COUNT; ++action) {
    snprintf(line, sizeof(line), "%c %s", action == g_system.selected ? '>' : ' ', action_label(action));
    minitel_ui_write_line(static_cast<uint8_t>(ACTIONS_ROW + action), line);
  }
  shell::render_status_line();
}

void system_screen_handle(const input_event_t& event) {
  const bool keeps_arm = event.type == INPUT_EVENT_ENTER;
  if (g_system.armed && !keeps_arm) {
    g_system.armed = false;  // any other key cancels the pending action
    shell::clear_status();
  }

  switch (event.type) {
    case INPUT_EVENT_UP:
      if (g_system.selected > 0) {
        --g_system.selected;
      }
      break;
    case INPUT_EVENT_DOWN:
      if (g_system.selected + 1 < SYSTEM_ACTION_COUNT) {
        ++g_system.selected;
      }
      break;
    case INPUT_EVENT_ESCAPE:
      shell::switch_screen(SCREEN_HOME);
      return;
    case INPUT_EVENT_ENTER:
      if (g_system.selected == SYSTEM_ACTION_UPDATE) {
        run_update_action();
      } else {
        run_restart_action();
      }
      break;
    default:
      return;
  }
  shell::request_redraw();
}

void system_screen_show_update_progress(const char* message, int percent) {
  char line[UI_COLS + 1];
  if (percent >= 0) {
    snprintf(line, sizeof(line), "%.32s %d%%", message, percent);
  } else {
    snprintf(line, sizeof(line), "%.40s", message);
  }
  minitel_ui_write_line(UI_STATUS_ROW, line);
  minitel_ui_flush_rows(UI_STATUS_ROW, UI_STATUS_ROW);
}
