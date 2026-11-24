// Runs the box's iambic keyer against scripted paddle movements and checks what comes out.
// Build and run: gcc -Ifirmware/src -o iambic_test tests/host/iambic_test.c firmware/src/iambic.c && ./iambic_test
#include <stdio.h>
#include <string.h>
#include "iambic.h"

#define UNIT 60000   // 20 WPM: a dit is 60 ms
typedef struct { int at_ms; int dit, dah; } move_t;   // from this time on, the paddles are like this

// Plays the moves and writes what was keyed as dots and dashes.
static const char *run(const move_t *moves, int count, bool mode_b) {
  static char out[64]; int n = 0; iambic_t k = {0};
  bool dit = false, dah = false, was = false; int64_t down_at = 0;
  for (int ms = 0, m = 0; ms < 3000; ms++) {
    while (m < count && moves[m].at_ms <= ms) { dit = moves[m].dit; dah = moves[m].dah; m++; }
    bool key = iambic_step(&k, dit, dah, (int64_t)ms * 1000, UNIT, mode_b);
    if (key && !was) down_at = ms;
    if (!key && was) out[n++] = (ms - down_at) > 120 ? '-' : '.';
    was = key;
  }
  out[n] = 0; return out;
}

static int failed;
static void check(const char *what, const char *got, const char *want) {
  printf("%-4s %-58s %-6s %s\n", strcmp(got, want) ? "FAIL" : "ok", what, got, strcmp(got, want) ? want : "");
  if (strcmp(got, want)) failed = 1;
}

int main(void) {
  // dit lasts 0-60, gap 60-120; dah 120-300, gap 300-360; dit 360-420 ...
  move_t tap_dit[] = { {0, 1, 0}, {30, 0, 0} };
  check("one tap on the dit paddle, A", run(tap_dit, 2, false), ".");
  check("one tap on the dit paddle, B", run(tap_dit, 2, true), ".");
  move_t hold_dit[] = { {0, 1, 0}, {200, 0, 0} };
  check("dit paddle held for 200 ms", run(hold_dit, 2, true), "..");
  move_t hold_dah[] = { {0, 0, 1}, {450, 0, 0} };
  check("dah paddle held for 450 ms", run(hold_dah, 2, false), "--");

  // The textbook difference: squeeze, dit first, let go of both during the first dah.
  move_t squeeze_dah[] = { {0, 1, 0}, {10, 1, 1}, {200, 0, 0} };
  check("squeeze, let go during the dah, A: ends with the dah", run(squeeze_dah, 3, false), ".-");
  check("squeeze, let go during the dah, B: one more dit", run(squeeze_dah, 3, true), ".-.");
  // Let go of both during the first dit: the dah was squeezed, so B adds it; A does too, because the dah
  // paddle was pressed afresh during the dit.
  move_t squeeze_dit[] = { {0, 1, 0}, {10, 1, 1}, {40, 0, 0} };
  check("squeeze, let go during the first dit, A", run(squeeze_dit, 3, false), ".-");
  check("squeeze, let go during the first dit, B", run(squeeze_dit, 3, true), ".-");
  // C (-.-.) by squeezing dah first: in B let go during the second dah, in A hold into the last dit.
  move_t c_b[] = { {0, 0, 1}, {10, 1, 1}, {450, 0, 0} };
  check("C in B: dah first, let go during the 2nd dah", run(c_b, 3, true), "-.-.");
  move_t c_a[] = { {0, 0, 1}, {10, 1, 1}, {650, 0, 0} };
  check("C in A: dah first, hold into the last dit", run(c_a, 3, false), "-.-.");
  check("the same timing as B's C, in A, stops a dit short", run(c_b, 3, false), "-.-");
  // A short tap on the other paddle during an element is remembered in both modes (dit memory).
  move_t n_tap[] = { {0, 0, 1}, {60, 1, 1}, {100, 0, 1}, {150, 0, 0} };
  check("dah held, short dit tap during it, A", run(n_tap, 4, false), "-.");
  check("dah held, short dit tap during it, B", run(n_tap, 4, true), "-.");
  // Both paddles at exactly the same instant from rest: dit first.
  move_t both[] = { {0, 1, 1}, {30, 0, 0} };
  check("both at once from rest, A: the dit only", run(both, 2, false), ".");
  check("both at once from rest, B: dit then dah", run(both, 2, true), ".-");
  return failed;
}
