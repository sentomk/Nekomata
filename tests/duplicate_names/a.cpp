// a.cpp — one of two TUs defining a same-named static. The reload offers
// fresh objects compiled from this TU alone; the same-named static in b.cpp
// must NEVER be affected.

static int value() {
  return 1;
}

int a_value() {
  return value();
}
