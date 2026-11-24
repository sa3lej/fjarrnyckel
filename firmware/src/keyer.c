// Reads the key jack every millisecond and decides when the key is down.
// Straight key: the tip contact is the key. Paddle: an iambic keyer makes dits and dahs at the set speed.
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "fn.h"
#include "iambic.h"

volatile bool key_down;

// The sidetone follows key_down. The radio gets every change with its time, so the receiver can replay it exactly.
static bool radio_down;
static void to_radio(bool down) {
  if (down == radio_down) return;
  radio_down = down;
  link_edge(esp_timer_get_time(), down);
}

// A contact counts as changed once it has read the same for 3 ms; that hides contact bounce.
typedef struct { bool state; uint8_t count; } contact_t;
static bool settle(contact_t *c, bool raw) {
  if (raw == c->state) c->count = 0;
  else if (++c->count >= 3) { c->state = raw; c->count = 0; }
  return c->state;
}

static void keyer_task(void *arg) {
  contact_t tip = {0}, ring = {0};
  iambic_t iambic = {0};
  bool armed = false;      // a key that is already down when the box starts counts for nothing until it has been up
  TickType_t tick = xTaskGetTickCount();
  for (;;) {
    vTaskDelayUntil(&tick, 1);
    bool t = settle(&tip, gpio_get_level(PIN_TIP) == 0), r = settle(&ring, gpio_get_level(PIN_RING) == 0);
    if (!armed) { if (t || r) continue; armed = true; }
    bool training = settings.key_target == 1, paddle = settings.key_type != 0;
    bool dit = settings.swap ? r : t, dah = settings.swap ? t : r, keyed;
    if (!paddle) { keyed = t; iambic = (iambic_t){0}; }
    else keyed = iambic_step(&iambic, dit, dah, esp_timer_get_time(), 1200000 / (training ? usb_wpm() : settings.wpm ? settings.wpm : 20),
                             training ? usb_iambic() : settings.iambic_b);
    if (!training) { key_down = keyed; to_radio(keyed); usb_key(false, false, false); continue; }
    // Training: nothing goes to the radio. With raw paddles the site makes the elements and the tone.
    to_radio(false);
    bool raw = paddle && usb_raw();
    key_down = raw ? false : keyed;
    usb_key(raw && dit, raw && dah, !raw && keyed);
  }
}

void keyer_start(void) {
  gpio_config_t in = { .pin_bit_mask = (1ULL << PIN_TIP) | (1ULL << PIN_RING), .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE };
  gpio_config(&in);
  xTaskCreatePinnedToCore(keyer_task, "keyer", 3072, NULL, 21, NULL, 1);
}
