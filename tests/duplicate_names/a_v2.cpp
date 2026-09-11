// The edited form of a.cpp. Its two new static functions happen to share
// names with b.cpp, so the fresh object's symbol set overlaps b.cpp more than
// its actual source. Source identity must win over that coincidence.

static int value() {
  return 100;
}

static int decoy_one() {
  return 0;
}

static int decoy_two() {
  return 0;
}

int a_value() {
  return value() + decoy_one() + decoy_two();
}
