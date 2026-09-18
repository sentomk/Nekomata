/* basic.c — /Od shape probe: every construct maps to one COFF feature the
 * object_file reader must classify. Keep the constructs independent so a
 * dumped structure can be attributed to exactly one line. */

int warm_counter = 7;            /* .data definition, external */
int cold_counter;                /* .bss definition, external */
static int file_local = 3;       /* STATIC storage class, .data */

extern int external_source(int); /* undefined symbol → REL32 call site */

int add_probe(int a, int b) { return a + b; }

int call_probe(int x) {          /* intra-file call + global load */
  return add_probe(x, warm_counter) + external_source(x);
}

const char *label_probe(void) {  /* string literal → .rdata, RIP-relative */
  return "warm literal";
}

int *address_probe(void) {       /* address-of global → which reloc type? */
  return &warm_counter;
}
