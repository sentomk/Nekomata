// Cross-TU hot member C (baseline). The published version introduces c_new,
// a symbol with no live definition in the baseline process.

static int s_c = 0;

extern "C" int c_value() {
  ++s_c;
  return s_c;
}
