// shapes.cpp — C++ shape fixture (freestanding, no library headers, so the
// same source cross-compiles on every host): inline functions place their
// bodies in COMDAT text sections on both drivers.

extern int cold_counter; // defined below; the destructor only needs it here

struct widget {
  int v;
  widget(int n) : v(n) {}
  ~widget() { cold_counter += v; } // implicitly inline: COMDAT text
};

int cold_counter = 0;
static int file_local_counter = 1;

inline int inline_scale(int x) {
  return x * 3;
} // COMDAT text

int method_value(const widget& w) {
  return inline_scale(w.v) + file_local_counter;
}

int unwind_probe(int x) { // a local destructor keeps the unwind tables busy
  widget w(x);
  return w.v;
}

__declspec(selectany) int shared_counter = 5; // COMDAT data on both drivers
