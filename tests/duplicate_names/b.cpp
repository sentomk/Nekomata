// b.cpp — the other TU with the identical static name. If the loader
// matched symbols by name alone, patching a.cpp's object could land HERE.

static int value() {
  return 2;
}

// The edited a.cpp deliberately grows functions with these same names. A
// symbol-overlap guess would then decide that a.cpp's fresh object came from
// this file and redirect this file's statics instead.
static int decoy_one() {
  return 0;
}

static int decoy_two() {
  return 0;
}

int b_value() {
  return value() + decoy_one() + decoy_two();
}
