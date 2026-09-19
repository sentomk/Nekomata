/* exec.c — load-and-run fixture: no mutable state, so the state path never
 * engages. One call to a provider-resolved external, one to a sibling only
 * the generation can define, and one read from placed read-only data. */

extern int exec_external(int); /* the provider resolves this */
extern int exec_sibling(int);  /* left pending for link_generation() */

const int exec_scale = 3; /* rip-relative read from the image's rodata */

int exec_plain(int x) { /* resolved call plus a const load */
  return exec_external(x) + exec_scale;
}

int exec_pending(int x) { /* the generation must define the target */
  return exec_sibling(x);
}
