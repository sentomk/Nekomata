#include "shared.hpp"

static int helper() {
  return 1;
}

namespace {
int hidden() {
  return 3;
}
} // namespace

int overloaded(int value) {
  return value + 1;
}

double overloaded(double value) {
  return value + 0.5;
}

int sample::widget::compute() const {
  return value + 2;
}

int from_a() {
  const sample::widget object{7};
  return helper() + hidden() + overloaded(1) + static_cast<int>(overloaded(1.0)) + object.compute();
}
