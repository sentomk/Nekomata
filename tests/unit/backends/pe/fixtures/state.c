/* state.c — mutable-state fixture: one existing external global bound to
 * live storage, one fresh external, one fresh file-local. */

extern int state_live; /* the fake state manager maps this to the test's */

int state_fresh = 7;        /* fresh storage, initialized bytes copied */
int state_counter;          /* common record: zeroed fresh storage */
static int state_local = 3; /* needs a source identity to be fresh */

int state_entry(int x) {
  state_counter += 1; /* post-increment value is part of the result */
  return x + state_live + state_fresh + state_local + state_counter;
}
