// Heterogeneous unit B: plain hot TU, different compile settings from
// unit A, same atomic group.

static int s_extra = 0;

extern "C" int mixed_extra() {
  ++s_extra;
  return s_extra * 10 + 1; // v1: the units digit is the code version
}
