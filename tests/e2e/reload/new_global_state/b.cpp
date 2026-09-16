#include "state_api.hpp"

extern "C" int state_b_value() {
  return -1;
}

#ifndef STATE_OMIT_GUARD
__attribute__((naked)) void state_unpatchable_guard() {
  __asm__ volatile("nop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tret");
}
#endif
