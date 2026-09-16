#include "shared.hpp"

static int helper() {
  return 2;
}

namespace {
int hidden() {
  return 4;
}
} // namespace

int from_b() {
  return helper() + hidden();
}
