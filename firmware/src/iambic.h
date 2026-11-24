// The iambic keyer: turns the two paddle contacts into dits and dahs. No hardware in here, so it also runs in tests.
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
  enum { IAMBIC_IDLE, IAMBIC_MARK, IAMBIC_GAP } phase;
  bool sending_dah, dit_wanted, dah_wanted, dit_was, dah_was, key, running;
  int64_t until;
} iambic_t;

// Call every millisecond with the two contacts, the time, the length of a dit and the mode. Returns: key down.
bool iambic_step(iambic_t *k, bool dit, bool dah, int64_t now_us, int64_t unit_us, bool mode_b);
