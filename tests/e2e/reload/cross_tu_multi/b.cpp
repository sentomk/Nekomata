// Cross-TU hot member B (baseline). The published version introduces b_new,
// a symbol with no live definition in the baseline process.

static int s_b = 0;

extern "C" int b_value() {
  ++s_b;
  return s_b;
}
