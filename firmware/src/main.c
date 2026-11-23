// Fjärrnyckel: the operator's box of the remote CW keying link.
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "fn.h"

void app_main(void) {
  settings_load();
  audio_start();
  keyer_start();
  audio_beep(500, 120); vTaskDelay(pdMS_TO_TICKS(220)); audio_beep(700, 120);   // power-on signal: low, high
  net_start();
  link_start();
  ts_start();
  web_start();
  update_started_ok();
  ESP_LOGI("fn", "Fjarrnyckel %s started. Key type: %s", firmware_version(), settings.key_type ? "paddle" : "straight key");
}
