// Settings live in the "fn" NVS namespace as one blob, so adding a field later only needs a new default.
#include <string.h>
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "fn.h"

settings_t settings;

void settings_load(void) {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) { nvs_flash_erase(); nvs_flash_init(); }
  settings = (settings_t){ .key_type = 0, .swap = 0, .wpm = 20, .tone_hz = 650, .volume = 30 };
  nvs_handle_t h;
  if (nvs_open("fn", NVS_READONLY, &h) != ESP_OK) return;
  settings_t stored; size_t len = sizeof stored;
  if (nvs_get_blob(h, "settings", &stored, &len) == ESP_OK && len == sizeof stored) settings = stored;
  nvs_close(h);
  settings.ssid[sizeof settings.ssid - 1] = settings.pass[sizeof settings.pass - 1] = 0;
}

void settings_save(void) {
  nvs_handle_t h;
  if (nvs_open("fn", NVS_READWRITE, &h) != ESP_OK) { ESP_LOGE("settings", "cannot open flash storage"); return; }
  nvs_set_blob(h, "settings", &settings, sizeof settings);
  nvs_commit(h);
  nvs_close(h);
}
