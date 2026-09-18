/* basic.c — /Od shape fixture: every construct maps to one section-level
 * feature the COFF reader must classify. Keep the constructs independent so
 * a parsed structure can be attributed to exactly one line. */

int warm_counter = 7;      /* initialized data, the file's first */
int cold_counter;          /* uninitialized storage (or a common
                              symbol, which is invisible here) */
static int file_local = 3; /* internal linkage */

extern int external_source(int); /* undefined symbol, no storage */

int add_probe(int a, int b) {
  return a + b;
}

int internal_probe(int x) { /* touches the internal-linkage global */
  return x + file_local;
}

int call_probe(int x) { /* text: calls plus a global load */
  return add_probe(x, warm_counter) + external_source(x) + internal_probe(0);
}

const char* label_probe(void) { /* string literal: .rdata on clang-cl,
                                   plain .data on MSVC */
  return "warm literal";
}

int* address_probe(void) { /* RIP-relative address-of in text */
  return &warm_counter;
}
