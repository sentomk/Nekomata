#include "shared.hpp"

int b_value() {
  return neko_multi_tu_value;
}

#ifndef neko_multi_tu_omit_guard
__attribute__((naked)) void b_guard() {
  __asm__ volatile("nop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tret");
}
#endif
