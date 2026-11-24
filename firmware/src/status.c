// What the box tells without its page: the light on the board, and a short tone when keying comes on or goes off.
//   blue, blinking   the setup network is up            amber   Wi-Fi is there, the radio is not ready
//   red              no Wi-Fi                           green   ready: the key goes to the radio (bright while keyed)
//   violet, blinking an update is being installed       white   service mode
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "led_strip.h"
#include "fn.h"

// The light sits on GPIO48 on most ESP32-S3-DevKitC-1 boards and on GPIO38 on version 1.1; both are driven.
static led_strip_handle_t leds[2];

// The colours below are written for brightness 10; the setting scales them all, up to ten times as bright.
static int scaled(int v) {
  int level = settings.led_level >= 1 && settings.led_level <= 100 ? settings.led_level : 10;
  v = v * level / 10;
  return v > 255 ? 255 : v;
}

void led_show(int r, int g, int b) {
  r = scaled(r); g = scaled(g); b = scaled(b);
  for (int i = 0; i < 2; i++) if (leds[i]) { led_strip_set_pixel(leds[i], 0, r, g, b); led_strip_refresh(leds[i]); }
}

static void led_init(void) {
  static const int pins[2] = { 48, 38 };
  for (int i = 0; i < 2; i++) {
    led_strip_config_t strip = { .strip_gpio_num = pins[i], .max_leds = 1, .led_model = LED_MODEL_WS2812,
                                 .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB };
    led_strip_rmt_config_t rmt = { .resolution_hz = 10 * 1000 * 1000 };
    if (led_strip_new_rmt_device(&strip, &rmt, &leds[i]) != ESP_OK) leds[i] = NULL;
  }
  led_show(0, 0, 0);
}

static void tone(int hz, int ms) { audio_beep(hz, ms); vTaskDelay(pdMS_TO_TICKS(ms)); }
static void quiet(int ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

static void status_task(void *arg) {
  bool told_ready = false; int64_t since = 0; bool was = false;
  for (int n = 0;; n++) {
    vTaskDelay(pdMS_TO_TICKS(50));
    bool ready = link_info().state == LINK_READY && settings.key_target == 0, blink = (n / 8) % 2;
    if (settings.led_off) led_show(0, 0, 0);
    else if (update_info().progress >= 0) blink ? led_show(18, 0, 26) : led_show(0, 0, 0);
    else if (net_ap_name()[0]) blink ? led_show(0, 0, 30) : led_show(0, 0, 0);
    else if (net_state() != NET_CONNECTED) led_show(26, 0, 0);
    else if (settings.key_target == 1) key_down ? led_show(0, 70, 70) : led_show(0, 16, 18);   // training: turquoise
    else if (ready) key_down ? led_show(0, 90, 0) : led_show(0, 22, 0);
    else if (service_mode()) led_show(20, 20, 20);
    else led_show(26, 14, 0);

    // A tone only when the change has lasted (1 s for ready, 3 s for gone) and the key is up, so a blip in the
    // network does not chirp and nothing sounds on top of your own sending.
    int64_t now = esp_timer_get_time();
    if (ready != was) { was = ready; since = now; }
    if (settings.tones_off || key_down) continue;
    if (ready && !told_ready && now - since > 1000000) {
      told_ready = true;                                           // "R" in Morse: ready
      tone(880, 60); quiet(60); tone(880, 180); quiet(60); tone(880, 60);
    } else if (!ready && told_ready && now - since > 3000000) {
      told_ready = false;                                          // two low tones: keying is off
      tone(330, 150); quiet(100); tone(330, 150);
    }
  }
}

void status_start(void) { xTaskCreatePinnedToCore(status_task, "status", 4096, NULL, 3, NULL, 0); }
void led_start(void) { led_init(); }
