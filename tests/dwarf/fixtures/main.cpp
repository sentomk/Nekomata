#include "shared.hpp"

int main() {
  // Nonzero even when correct: inspecting must never execute this program.
  return from_a() + from_b() == 22 ? 73 : 74;
}
