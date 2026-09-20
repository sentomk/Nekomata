#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "protocol/wasm_offer.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace {

constexpr std::string_view a_digest =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr std::string_view b_digest =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

neko::detail::wasm_offer offer() {
  neko::detail::wasm_offer value;
  value.group_id = "//game:logic(//toolchain:wasm)";
  value.sequence = 17;
  value.generation_id = "gen-17-b2";
  value.abi_id = "wasm32-boids-v3";
  value.artifact_path = "modules/gen-17-b2.wasm";
  value.sha256 = std::string(a_digest);
  value.entries = {"update", "identity"};
  value.changed_inputs = {"//src:update.cpp"};
  return value;
}

neko::detail::wasm_offer trimmed() {
  auto value = offer();
  value.group_id = "game";
  value.sequence = 7;
  value.generation_id = "gen-7";
  value.abi_id = "wasm32-v1";
  value.artifact_path = "modules/gen-7.wasm";
  return value;
}

void check_error(std::string_view text, neko::detail::wasm_offer_error_code expected_code,
                 std::size_t expected_line, std::string_view expected_message) {
  try {
    static_cast<void>(neko::detail::parse_wasm_offer(text, "fixture"));
    FAIL("wasm offer must be rejected");
  } catch (const neko::detail::wasm_offer_error& error) {
    CHECK(error.code() == expected_code);
    CHECK(error.line() == expected_line);
    CHECK(std::string_view{error.what()} == expected_message);
  }
}

// Eight lines: header, four identity fields, the artifact row, two entries.
std::string base() {
  return "nekomata-wasm-v1\n"
         "group_id \"game\"\n"
         "sequence 7\n"
         "generation_id \"gen-7\"\n"
         "abi_id \"wasm32-v1\"\n"
         "artifact \"modules/gen-7.wasm\" \"" +
         std::string(a_digest) +
         "\"\n"
         "entry \"update\"\n"
         "entry \"identity\"\n";
}

std::string artifact_line(std::string_view path = "modules/gen-7.wasm",
                          std::string_view digest = a_digest) {
  return "artifact \"" + std::string(path) + "\" \"" + std::string(digest) + "\"\n";
}

} // namespace

TEST_CASE("wasm offers have a deterministic round trip") {
  const auto original = offer();
  const std::string encoded = neko::detail::serialize_wasm_offer(original);

  CHECK(encoded == "nekomata-wasm-v1\n"
                   "group_id \"//game:logic(//toolchain:wasm)\"\n"
                   "sequence 17\n"
                   "generation_id \"gen-17-b2\"\n"
                   "abi_id \"wasm32-boids-v3\"\n"
                   "artifact \"modules/gen-17-b2.wasm\" "
                   "\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"\n"
                   "entry \"update\"\n"
                   "entry \"identity\"\n"
                   "changed_input \"//src:update.cpp\"\n");
  CHECK(neko::detail::parse_wasm_offer(encoded, "round-trip") == original);
}

TEST_CASE("blank lines and whole-line comments are accepted") {
  auto noisy = "nekomata-wasm-v1\n"
               "\n"
               "# a whole-line comment\n" +
               base().substr(base().find('\n') + 1);
  noisy += "changed_input \"//src:update.cpp\"\n";
  CHECK(neko::detail::parse_wasm_offer(noisy, "noisy") == trimmed());
}

TEST_CASE("carriage returns at line ends are tolerated") {
  auto text = base();
  text.replace(text.find('\n'), 1, "\r\n");
  CHECK(neko::detail::parse_wasm_offer(text, "crlf").sequence == 7);
}

TEST_CASE("the format header decides format against version errors") {
  check_error("", neko::detail::wasm_offer_error_code::invalid_format, 1,
              "invalid wasm offer 'fixture' at line 1: missing format header");
  check_error("nekomata-wasm-v2\n", neko::detail::wasm_offer_error_code::unsupported_version, 1,
              "invalid wasm offer 'fixture' at line 1: expected nekomata-wasm-v1");
  check_error("nekomata-generation-v2\n", neko::detail::wasm_offer_error_code::invalid_format, 1,
              "invalid wasm offer 'fixture' at line 1: expected nekomata-wasm-v1");
}

TEST_CASE("duplicate scalar fields are rejected") {
  check_error(base() + "group_id \"again\"\n", neko::detail::wasm_offer_error_code::duplicate_field,
              9, "invalid wasm offer 'fixture' at line 9: duplicate group_id");
  check_error(base() + "sequence 8\n", neko::detail::wasm_offer_error_code::duplicate_field, 9,
              "invalid wasm offer 'fixture' at line 9: duplicate sequence");
  check_error(base() + artifact_line(), neko::detail::wasm_offer_error_code::duplicate_field, 9,
              "invalid wasm offer 'fixture' at line 9: duplicate artifact");
}

TEST_CASE("missing fields are reported after the last line") {
  auto text = base();
  const auto artifact_at = text.find("artifact");
  text.erase(artifact_at, text.find("entry") - artifact_at);
  check_error(text, neko::detail::wasm_offer_error_code::missing_field, 8,
              "invalid wasm offer 'fixture' at line 8: missing artifact");

  text = base();
  text.erase(text.find("entry"));
  check_error(text, neko::detail::wasm_offer_error_code::missing_field, 7,
              "invalid wasm offer 'fixture' at line 7: at least one entry is required");

  text = base();
  const auto abi_at = text.find("abi_id");
  const auto abi_end = text.find('\n', abi_at) + 1;
  text.erase(abi_at, abi_end - abi_at);
  check_error(text, neko::detail::wasm_offer_error_code::missing_field, 8,
              "invalid wasm offer 'fixture' at line 8: missing abi_id");
}

TEST_CASE("unknown directives and trailing fields are rejected") {
  check_error(base() + "object \"objects/ball.o\"\n",
              neko::detail::wasm_offer_error_code::unknown_directive, 9,
              "invalid wasm offer 'fixture' at line 9: unknown directive 'object'");

  auto text = base();
  text += "entry \"update\" \"extra\"\n";
  check_error(text, neko::detail::wasm_offer_error_code::unexpected_value, 9,
              "invalid wasm offer 'fixture' at line 9: unexpected trailing fields");
}

TEST_CASE("structural value errors keep their lines") {
  auto text = base();
  text += "entry update\n"; // unquoted
  check_error(text, neko::detail::wasm_offer_error_code::malformed_value, 9,
              "invalid wasm offer 'fixture' at line 9: entry must be quoted");

  check_error("nekomata-wasm-v1\n"
              "group_id \"game\"\n"
              "sequence 07\n",
              neko::detail::wasm_offer_error_code::malformed_value, 3,
              "invalid wasm offer 'fixture' at line 3: sequence must be a canonical unsigned "
              "decimal integer");
}

TEST_CASE("artifact rows carry a modules path and a lowercase digest") {
  auto text = base();
  text.replace(text.find("modules/gen-7.wasm"), 17, "objects/gen-7.wasm");
  check_error(text, neko::detail::wasm_offer_error_code::invalid_field, 6,
              "invalid wasm offer 'fixture' at line 6: artifact_path must be below modules/");

  text = base();
  text.replace(text.find(a_digest), 64, std::string(b_digest).substr(0, 63));
  check_error(text, neko::detail::wasm_offer_error_code::invalid_field, 6,
              "invalid wasm offer 'fixture' at line 6: sha256 must contain exactly 64 lowercase "
              "hexadecimal digits");

  text = base();
  std::string upper(a_digest);
  for (auto& byte : upper) {
    byte = static_cast<char>(byte - 32); // 'a'..'f' becomes 'A'..'F'
  }
  text.replace(text.find(a_digest), 64, upper);
  check_error(text, neko::detail::wasm_offer_error_code::invalid_field, 6,
              "invalid wasm offer 'fixture' at line 6: sha256 must contain exactly 64 lowercase "
              "hexadecimal digits");
}

TEST_CASE("entry and changed_input duplicates are rejected with their names") {
  auto text = base();
  text += "entry \"identity\"\n";
  check_error(text, neko::detail::wasm_offer_error_code::duplicate_entry, 9,
              "invalid wasm offer 'fixture' at line 9: duplicate entry 'identity'");

  text = base();
  text += "changed_input \"//src:update.cpp\"\n";
  text += "changed_input \"//src:update.cpp\"\n";
  check_error(text, neko::detail::wasm_offer_error_code::duplicate_changed_input, 10,
              "invalid wasm offer 'fixture' at line 10: duplicate changed_input "
              "'//src:update.cpp'");
}

TEST_CASE("serializing validates first") {
  auto broken = offer();
  broken.entries.clear();
  CHECK_THROWS_AS(static_cast<void>(neko::detail::serialize_wasm_offer(broken)),
                  neko::detail::wasm_offer_error);
  broken = offer();
  broken.artifact_path = "elsewhere/gen-7.wasm";
  CHECK_THROWS_AS(static_cast<void>(neko::detail::serialize_wasm_offer(broken)),
                  neko::detail::wasm_offer_error);
}
