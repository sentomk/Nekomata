// vtable.cpp — virtual-function shape probe: vftables store function
// addresses (expected ADDR64) and namespace-scope dynamic initializers add
// their own symbol family. Out of the supported set today; surveyed to
// choose the parser's rejection policy.
struct animal {
  virtual int speak(int x);  // vftable → function address in .data
  int base = 1;
};

int animal::speak(int x) { return x + base; }

animal build_animal(void) { return animal{}; }  // forces vftable emission

int runtime_seeded = build_animal().speak(2);  // dynamic initializer
