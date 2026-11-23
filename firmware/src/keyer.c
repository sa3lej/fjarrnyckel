// Reads the key jack every millisecond and decides when the key is down.
// Straight key: the tip contact is the key. Paddle: an iambic keyer makes dits and dahs at the set speed.
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "fn.h"

volatile bool key_down;

static void set_key(bool down) {
  if (down == key_down) return;
  key_down = down;
  link_edge(esp_timer_get_time(), down);   // the time goes with the edge, so the receiver can replay it exactly
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
  enum { IDLE, MARK, GAP } phase = IDLE;
  bool sending_dah = false, dit_wanted = false, dah_wanted = false, dit_was = false, dah_was = false;
  int64_t until = 0;
  bool armed = false;      // a key that is already down when the box starts counts for nothing until it has been up
  TickType_t tick = xTaskGetTickCount();
  for (;;) {
    vTaskDelayUntil(&tick, 1);
    bool t = settle(&tip, gpio_get_level(PIN_TIP) == 0), r = settle(&ring, gpio_get_level(PIN_RING) == 0);
    if (!armed) { if (t || r) continue; armed = true; }
    if (settings.key_type == 0) { set_key(t); phase = IDLE; continue; }

    bool dit = settings.swap ? r : t, dah = settings.swap ? t : r;
    int64_t now = esp_timer_get_time(), unit = 1200000 / (settings.wpm ? settings.wpm : 20);
    // The opposite paddle is remembered while an element goes out, and sent next.
    // Mode B remembers it if it is held at any moment of the element, so letting go of a squeeze still adds one
    // more element. Mode A only remembers a fresh press, so letting go of a squeeze ends with the current element.
    if (phase != IDLE) {
      if ((settings.iambic_b ? dit : dit && !dit_was) && sending_dah) dit_wanted = true;
      if ((settings.iambic_b ? dah : dah && !dah_was) && !sending_dah) dah_wanted = true;
    }
    dit_was = dit; dah_was = dah;
    if (phase == MARK && now >= until) { set_key(false); phase = GAP; until = now + unit; }
    if (phase == GAP && now >= until) phase = IDLE;
    if (phase == IDLE) {
      bool next_dah;
      if (dit_wanted || dah_wanted) next_dah = dah_wanted;              // the remembered, opposite element
      else if (dit && dah) next_dah = !sending_dah;                     // squeezed: alternate
      else if (dit || dah) next_dah = dah;
      else continue;
      dit_wanted = dah_wanted = false;
      sending_dah = next_dah; set_key(true); phase = MARK; until = now + unit * (next_dah ? 3 : 1);
    }
  }
}

void keyer_start(void) {
  gpio_config_t in = { .pin_bit_mask = (1ULL << PIN_TIP) | (1ULL << PIN_RING), .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE };
  gpio_config(&in);
  xTaskCreatePinnedToCore(keyer_task, "keyer", 3072, NULL, 21, NULL, 1);
}
