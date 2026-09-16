// Hot TU of the CMake adapter fixture. The reload script edits the version
// digit; the tick counter is state that must survive every swap.

static int s_ticks = 0;

extern "C" int adapter_tick() {
  ++s_ticks;
  return s_ticks * 10 + 1; // v1: the units digit is the code version
}
