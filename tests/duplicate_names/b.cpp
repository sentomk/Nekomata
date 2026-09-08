// b.cpp — the other TU with the identical static name. If the loader
// matched symbols by name alone, patching a.cpp's object could land HERE.

static int value() {
  return 2;
}

int b_value() {
  return value();
}
