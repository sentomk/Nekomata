// hot.cpp — baseline TU for the rejection-cases harness. Deliberately a
// single function: a failed entry patch on a one-function image leaves the
// process in a clean, unpatched state (multi-function transactionality is
// a documented Phase 2 boundary).

#include <cstdio>

int g_counter = 0;
static int s_calls = 0;

void tick() {
  ++g_counter;
  ++s_calls;
  std::printf("[hot] tick #%d, g_counter=%d\n", s_calls, g_counter);
}
