// Cross-TU hot member B (baseline). The naked guard stays in the baseline
// link; publishing a fresh object that defines it again must reject the
// whole generation, because its entry has no recognizable prologue.

#include "cross_tu_sources.hpp"

static int s_b = 0;

extern "C" int b_value() {
  ++s_b;
  return s_b;
}

#ifndef CROSS_TU_OMIT_GUARD
__attribute__((naked)) void b_guard() {
  __asm__ volatile("nop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tret");
}
#endif
