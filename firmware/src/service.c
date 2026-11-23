// Holding the key down while the box is plugged in is the owner's way back in, with nothing but the key:
//   let go after the first beep (5 s)      -> service mode: the page's password is removed, and until the next
//                                             restart the box also accepts firmware that is not signed
//   let go after the three beeps (15 s)    -> everything is erased: the box is as new
//   never let go (25 s)                    -> nothing; that is a key that is stuck or switched closed
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "fn.h"

static bool service;
bool service_mode(void) { return service; }

static bool held(void) { return gpio_get_level(PIN_TIP) == 0; }
static void beeps(int count, int hz) { for (int i = 0; i < count; i++) { audio_beep(hz, 120); vTaskDelay(pdMS_TO_TICKS(260)); } }

void service_check(void) {
  gpio_config_t in = { .pin_bit_mask = 1ULL << PIN_TIP, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE };
  gpio_config(&in);
  vTaskDelay(pdMS_TO_TICKS(30));
  if (!held()) return;
  ESP_LOGW("service", "the key is down at start: 5 s for service mode, 15 s to erase everything");
  int64_t start = esp_timer_get_time(); int stage = 0;
  while (held()) {
    int64_t s = (esp_timer_get_time() - start) / 1000000;
    if (s >= 25) { ESP_LOGW("service", "the key never came up: nothing is changed"); return; }
    if (s >= 15 && stage < 2) { stage = 2; led_show(40, 0, 0); beeps(3, 900); }
    else if (s >= 5 && stage < 1) { stage = 1; led_show(30, 30, 30); beeps(1, 900); }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  if (stage == 2) {
    ESP_LOGW("service", "erasing all settings");
    beeps(2, 400);
    nvs_flash_deinit(); nvs_flash_erase();
    esp_restart();
  }
  if (stage == 1) {
    service = true;
    if (settings.page_lock) { settings.page_lock = 0; settings_save(); }
    ESP_LOGW("service", "service mode until the next restart: no page password, unsigned firmware is accepted");
    beeps(2, 1200);
  }
}
