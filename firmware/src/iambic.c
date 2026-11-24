#include "iambic.h"

// One paddle held repeats its element; both held (a squeeze) alternates dit and dah.
// While an element goes out, the opposite paddle is remembered and sent next:
//   Mode B remembers it if it is held at any moment of the element, so letting go of a squeeze still adds one
//   more element. Mode A only remembers a fresh press, so letting go of a squeeze ends with the current element,
//   while a short tap during an element is still not lost.
bool iambic_step(iambic_t *k, bool dit, bool dah, int64_t now, int64_t unit, bool mode_b) {
  if (k->phase != IAMBIC_IDLE) {
    if ((mode_b ? dit : dit && !k->dit_was) && k->sending_dah) k->dit_wanted = true;
    if ((mode_b ? dah : dah && !k->dah_was) && !k->sending_dah) k->dah_wanted = true;
  }
  k->dit_was = dit; k->dah_was = dah;
  if (k->phase == IAMBIC_MARK && now >= k->until) { k->key = false; k->phase = IAMBIC_GAP; k->until = now + unit; }
  if (k->phase == IAMBIC_GAP && now >= k->until) k->phase = IAMBIC_IDLE;
  if (k->phase == IAMBIC_IDLE) {
    bool next_dah;
    if (k->dit_wanted || k->dah_wanted) next_dah = k->dah_wanted;       // the remembered, opposite element
    else if (dit && dah) next_dah = k->running ? !k->sending_dah : false; // squeezed: alternate; from rest, dit first
    else if (dit || dah) next_dah = dah;
    else { k->running = false; return k->key; }
    k->running = true;
    k->dit_wanted = k->dah_wanted = false;
    k->sending_dah = next_dah; k->key = true; k->phase = IAMBIC_MARK; k->until = now + unit * (next_dah ? 3 : 1);
  }
  return k->key;
}
