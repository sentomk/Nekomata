// Cross-TU hot member D (baseline). The published version introduces d_new,
// a symbol with no live definition in the baseline process.

static int s_d = 0;

extern "C" int d_value() {
  ++s_d;
  return s_d;
}
