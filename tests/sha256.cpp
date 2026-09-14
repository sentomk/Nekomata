#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "runtime/sha256.hpp"

#include <string>
#include <string_view>

namespace {

std::string hex_of(std::string_view text) {
  return neko::detail::sha256_hex(text);
}

} // namespace

TEST_CASE("SHA-256 matches the FIPS 180-4 example vectors") {
  CHECK(hex_of("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(hex_of("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK(hex_of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  CHECK(hex_of(std::string(1'000'000, 'a')) ==
        "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("the digest and hexadecimal forms agree") {
  const std::string text = "nekomata generation stream";
  const auto digest = neko::detail::sha256_digest(
      {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
  REQUIRE(digest.size() == 32);

  static constexpr char k_digits[] = "0123456789abcdef";
  std::string expected;
  for (const auto byte : digest) {
    expected.push_back(k_digits[byte >> 4]);
    expected.push_back(k_digits[byte & 0x0f]);
  }
  CHECK(neko::detail::sha256_hex(text) == expected);
  CHECK(neko::detail::sha256_hex(text).size() == 64);
}
