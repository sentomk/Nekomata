// root.cpp — cross-object fixture: the root calls a sibling-only symbol.
// Freestanding C++ (cross-compiles on every host, see shapes.cpp).

extern "C" int cross_sibling(int x);

extern "C" int cross_root(int x) {
  return cross_sibling(x) * 2;
}
