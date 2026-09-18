/* relocs.c — relocation-shape fixture: one construct per deterministic
 * relocation category at the drivers' /Od defaults. */

int reloc_global = 11;      /* initialized data */
static int reloc_local = 2; /* internal linkage */

extern int reloc_external(int); /* undefined: REL32 call site in text */

int* reloc_address_of = &reloc_global; /* .data: ADDR64 against a defined
                                          symbol */

int reloc_call_site(int x) { /* text: REL32 call plus RIP-relative loads */
  return reloc_external(x) + reloc_global + reloc_local;
}
