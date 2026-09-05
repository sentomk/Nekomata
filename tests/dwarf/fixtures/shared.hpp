#pragma once

int from_a();
int from_b();
int overloaded(int value);
double overloaded(double value);

namespace sample {
struct widget {
  int value;
  int compute() const;
};
} // namespace sample
