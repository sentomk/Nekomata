#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "runtime/descriptor_discovery.hpp"

TEST_CASE("a program without an embedded section discovers no groups") {
  const std::uint8_t* begin = nullptr;
  const std::uint8_t* end = nullptr;
  CHECK_FALSE(neko::detail::embedded_descriptor_section(begin, end));
  CHECK(neko::detail::discover_embedded_descriptors().empty());
}
