// sibling.cpp — defines the symbol root.cpp imports.

extern "C" int cross_sibling(int x) {
  return x + 1;
}

// A live-process anchor for this object's reload; cross_sibling itself
// stays fresh so only a generation sibling can define it.
extern "C" int sibling_anchor(int x) {
  return x;
}
