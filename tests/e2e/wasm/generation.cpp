#include "contract.hpp"

namespace {

std::uint32_t identify() {
  return GENERATION_ID;
}

const generation_descriptor descriptor{1, GENERATION_ID, identify};

} // namespace

extern "C" const generation_descriptor* get_generation_descriptor() {
  return &descriptor;
}
