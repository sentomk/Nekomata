// unwind.cpp — exception fixture: a fresh frame with a handler whose catch
// unwinds through the dynamically registered tables. The throwing helper
// stays in the host process; only the frame with the handler reloads.

extern "C" int unwind_thrower(int x); // resolved to the test process

struct guard { // a real destructor keeps the unwind tables honest
  int* sum;
  explicit guard(int* s) : sum(s) {}
  ~guard() { *sum += 1; }
};

extern "C" int unwind_entry(int x) {
  int destroyed = 0;
  guard g(&destroyed); // outside the try: unwinding must destruct it
  try {
    return unwind_thrower(x);
  } catch (int value) {
    return value + destroyed * 100;
  }
}
