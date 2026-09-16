// hot.cpp — the soak harness TU. Variants change kStep/kLabel (logic-only,
// fresh rodata each reload); the mutable state below must survive every
// generation with exact continuity.

#include <cstdio>

int g_counter = 0;
static int s_calls = 0;

const int kStep = 1;        // variants: 2, 3, 4, ...
const char kLabel[] = "g0"; // variants: g1, g2, ...

void tick() {
  g_counter += kStep;
  ++s_calls;
  std::printf("[%s] tick g=%d s=%d\n", kLabel, g_counter, s_calls);
}
