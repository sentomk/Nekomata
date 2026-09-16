// Cross-TU hot member A (baseline). The published version calls three fresh
// symbols that exist nowhere in the baseline binary, one per sibling unit —
// three pending cross-object call fixups resolved by link_generation().

static int s_a = 0;

extern "C" int a_value() {
  ++s_a;
  return s_a * 10 + 1; // v1: the units digit is the code version
}
