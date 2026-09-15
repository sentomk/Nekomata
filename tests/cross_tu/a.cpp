// Cross-TU hot member A (baseline). Later versions call into B's fresh
// definitions, including one that exists nowhere in the baseline binary.

static int s_a = 0;

extern "C" int a_value() {
  ++s_a;
  return s_a * 10 + 1; // v1: the units digit is the code version
}
