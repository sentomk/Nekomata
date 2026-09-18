// shapes.cpp — C++ shape probe (freestanding, -nostdinc++): MSVC mangling,
// COMDAT families, static storage, and unwind-info emission. No library
// headers so the same source compiles under `--target=x86_64-pc-windows-msvc`
// on any host.

extern int cold_counter;  // defined below; the destructor only needs it here

struct widget {
  int v;
  widget(int n) : v(n) {}
  ~widget() { cold_counter += v; }  // implicitly inline → COMDAT
};

int cold_counter = 0;
static int file_local_counter = 1;

inline int inline_scale(int x) { return x * 3; }  // inline → COMDAT selection?

int method_value(const widget &w) {  // MSVC-mangled free function
  return inline_scale(w.v) + file_local_counter;
}

int unwind_probe(int x) {  // local with destructor → .xdata/.pdata?
  widget w(x);
  return w.v;
}

__declspec(selectany) int shared_counter = 5;  // COMDAT data?
