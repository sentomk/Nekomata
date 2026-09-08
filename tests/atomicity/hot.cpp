// hot.cpp — atomicity harness TU. Three functions with distinct fates:
//   tick, tock        — normal -O0 functions, patchable
//   naked_trouble     — naked asm entry (nop; ret), deliberately NOT a
//                       recognized prologue: patch_entry refuses it, which
//                       makes it the deterministic mid-list failure needed
//                       to expose non-atomic patching.

#include <cstdio>

int g_counter = 0;

void tick() {
  ++g_counter;
  std::printf("[tick v1] %d\n", g_counter); // variants edit this line
}

void tock() {
  std::printf("[tock v1] %d\n", g_counter); // variants edit or drop this
}

__attribute__((naked)) void naked_trouble() {
  __asm__ volatile("nop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tret");
}
