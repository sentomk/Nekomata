// The hot TU of the managed end-to-end test. The publish script edits the
// version digit and republishes the group; the tick counter is state that
// must survive every reload.

static int s_ticks = 0;

extern "C" int managed_tick() {
  ++s_ticks;
  return s_ticks * 10 + 1; // v1: the units digit is the code version
}
