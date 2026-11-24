// Settings live in the "fn" NVS namespace as one blob, so adding a field later only needs a new default.
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_app_desc.h"
#include "microlink.h"
#include "fn.h"
#include "backup.h"

settings_t settings;

static const settings_t defaults = { .key_type = 0, .swap = 0, .wpm = 20, .tone_hz = 650, .volume = 30, .link_port = 7373, .iambic_b = 1, .hp_volume = 15 };

// Text fields always end inside their arrays, whatever was stored.
static void terminate(settings_t *s) {
  s->ssid[sizeof s->ssid - 1] = s->pass[sizeof s->pass - 1] = 0;
  s->link_host[sizeof s->link_host - 1] = s->link_key[sizeof s->link_key - 1] = 0;
  s->ts_key[sizeof s->ts_key - 1] = 0;
}

void settings_load(void) {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) { nvs_flash_erase(); nvs_flash_init(); }
  settings = defaults;
  nvs_handle_t h;
  if (nvs_open("fn", NVS_READONLY, &h) != ESP_OK) return;
  // A copy stored by older firmware is shorter: it fills the start and the newer fields keep their defaults.
  size_t len = 0;
  if (nvs_get_blob(h, "settings", NULL, &len) == ESP_OK && len && len <= sizeof settings) nvs_get_blob(h, "settings", &settings, &len);
  nvs_close(h);
  terminate(&settings);
}

void settings_save(void) {
  nvs_handle_t h;
  if (nvs_open("fn", NVS_READWRITE, &h) != ESP_OK) { ESP_LOGE("settings", "cannot open flash storage"); return; }
  nvs_set_blob(h, "settings", &settings, sizeof settings);
  nvs_commit(h);
  nvs_close(h);
}

// ---- backups ----
// The sealed contents: firmware version (16) | 1 if the Tailscale identity is included (1) | 0 (1)
// | length of the settings (2, little-endian) | Tailscale keys (6 x 32) | the settings blob.
// tools/backup.py reads the same layout.
typedef struct __attribute__((packed)) {
  char version[16];
  uint8_t has_ts, zero;
  uint16_t settings_len;
  uint8_t ts[6][32];
} contents_t;

void backup_yield(void) { vTaskDelay(1); }

int settings_backup(const char *pass, uint8_t *out, size_t size) {
  static uint8_t plain[sizeof(contents_t) + sizeof(settings_t)];
  contents_t c = { .settings_len = sizeof settings };
  strlcpy(c.version, esp_app_get_description()->version, sizeof c.version);
  c.has_ts = microlink_identity_export(c.ts) == ESP_OK;
  memcpy(plain, &c, sizeof c);
  memcpy(plain + sizeof c, &settings, sizeof settings);
  int n = backup_seal(pass, plain, sizeof plain, out, size);
  memset(plain, 0, sizeof plain); memset(&c, 0, sizeof c);
  return n;
}

int settings_restore(const char *pass, const uint8_t *file, size_t len) {
  if (len < BACKUP_OVERHEAD) return -1;
  uint8_t *plain = malloc(len);
  if (!plain) return -3;
  int n = backup_open(pass, file, len, plain, len);
  int result = n;
  contents_t c;
  if (n >= 0 && (n < (int)sizeof c || n < (int)(sizeof c + ((contents_t *)plain)->settings_len))) result = -1;
  else if (n >= 0) {
    memcpy(&c, plain, sizeof c);
    // A backup from older firmware is shorter: the newer settings keep their defaults. One from newer firmware
    // is longer: the settings this firmware does not know are left out.
    settings_t s = defaults;
    memcpy(&s, plain + sizeof c, c.settings_len < sizeof s ? c.settings_len : sizeof s);
    terminate(&s);
    if (c.has_ts && microlink_identity_import(c.ts) != ESP_OK) result = -3;
    else { settings = s; settings_save(); result = 0; }
    ESP_LOGI("settings", "restored a backup made by firmware %.16s%s", c.version, c.has_ts ? ", with the Tailscale identity" : "");
    memset(&s, 0, sizeof s); memset(&c, 0, sizeof c);
  }
  memset(plain, 0, len); free(plain);
  return result;
}
