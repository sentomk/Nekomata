#include <limits>

int main(int argc, char**) {
  // Deliberately exercise UBSan recovery policy, with a runtime operand so the
  // compiler cannot fold the operation away. Only built for sanitizer tests.
  volatile int value = std::numeric_limits<int>::max();
  volatile int result = value + argc;
  static_cast<void>(result);
  return 0;
}
