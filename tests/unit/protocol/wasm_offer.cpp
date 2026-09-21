#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "protocol/wasm_offer.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace {

constexpr std::string_view a_digest =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

neko::detail::wasm_offer offer() {
  neko::detail::wasm_offer value;
  value.group_id = "//game:logic(//toolchain:wasm)";
  value.sequence = 17;
  value.generation_id = "gen-17-b2";
  value.abi_id = "wasm32-boids-v3";
  value.artifact_path = "modules/gen-17-b2.wasm";
  value.sha256 = a_digest;
  value.entries = {"update", "identity"};
  return value;
}

// Seven lines: header, four identity fields, the artifact row, one entry.
std::string base() {
  return "nekomata-wasm/1\n"
         "group_id \"game\"\n"
         "sequence 7\n"
         "generation_id \"gen-7\"\n"
         "abi_id \"wasm32-v1\"\n"
         "artifact \"modules/gen-7.wasm\" \"" +
         std::string(a_digest) +
         "\"\n"
         "entry \"update\"\n";
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

} // namespace

TEST_CASE("wasm offers have a deterministic round trip") {
  const auto original = offer();
  const std::string encoded = neko::detail::serialize_wasm_offer(original);

  CHECK(encoded == "nekomata-wasm/1\n"
                   "group_id \"//game:logic(//toolchain:wasm)\"\n"
                   "sequence 17\n"
                   "generation_id \"gen-17-b2\"\n"
                   "abi_id \"wasm32-boids-v3\"\n"
                   "artifact \"modules/gen-17-b2.wasm\" "
                   "\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"\n"
                   "entry \"update\"\n"
                   "entry \"identity\"\n");
  CHECK(neko::detail::parse_wasm_offer(encoded, "round-trip") == original);
}

TEST_CASE("blank lines and whole-line comments are accepted but not serialized") {
  const auto expected = offer();
  auto encoded = neko::detail::serialize_wasm_offer(expected);
  const auto after_header = encoded.find('\n') + 1;
  encoded.insert(after_header, "\n  # producer diagnostic\n");
  CHECK(neko::detail::parse_wasm_offer(encoded, "comments") == expected);
}

TEST_CASE("format and version failures have stable codes") {
  check_error("", neko::detail::wasm_offer_error_code::invalid_format, 1,
              "invalid wasm offer 'fixture' at line 1: missing format header");
  check_error("not-an-offer\n", neko::detail::wasm_offer_error_code::invalid_format, 1,
              "invalid wasm offer 'fixture' at line 1: expected nekomata-wasm/1");
  check_error("nekomata-wasm/2\n", neko::detail::wasm_offer_error_code::unsupported_version, 1,
              "invalid wasm offer 'fixture' at line 1: expected nekomata-wasm/1");
  check_error("nekomata-wasm-v1\n", neko::detail::wasm_offer_error_code::unsupported_version, 1,
              "invalid wasm offer 'fixture' at line 1: expected nekomata-wasm/1");
  check_error("nekomata-wasm 1\n", neko::detail::wasm_offer_error_code::unsupported_version, 1,
              "invalid wasm offer 'fixture' at line 1: expected nekomata-wasm/1");
  check_error("nekomata-wasm\n", neko::detail::wasm_offer_error_code::unsupported_version, 1,
              "invalid wasm offer 'fixture' at line 1: expected nekomata-wasm/1");
  check_error("nekomata-generation/2\n", neko::detail::wasm_offer_error_code::invalid_format, 1,
              "invalid wasm offer 'fixture' at line 1: expected nekomata-wasm/1");
}

TEST_CASE("scalar and row failures are classified") {
  check_error(base() + "sequence 8\n", neko::detail::wasm_offer_error_code::duplicate_field, 8,
              "invalid wasm offer 'fixture' at line 8: duplicate sequence");
  check_error("nekomata-wasm/1\ngroup_id \"game\"\n",
              neko::detail::wasm_offer_error_code::missing_field, 3,
              "invalid wasm offer 'fixture' at line 3: missing sequence");
  check_error(base() + "extra \"x\"\n", neko::detail::wasm_offer_error_code::unknown_directive, 8,
              "invalid wasm offer 'fixture' at line 8: unknown directive 'extra'");
  check_error(base() + "entry \"update\"\n", neko::detail::wasm_offer_error_code::duplicate_entry,
              8, "invalid wasm offer 'fixture' at line 8: duplicate entry 'update'");

  auto short_digest = base();
  short_digest.replace(short_digest.find(a_digest), a_digest.size(), a_digest.substr(0, 63));
  check_error(short_digest, neko::detail::wasm_offer_error_code::invalid_field, 6,
              "invalid wasm offer 'fixture' at line 6: sha256 must contain exactly 64 lowercase "
              "hexadecimal digits");

  auto absolute = base();
  absolute.replace(absolute.find("modules/"), 8, "/modules/");
  check_error(absolute, neko::detail::wasm_offer_error_code::invalid_field, 6,
              "invalid wasm offer 'fixture' at line 6: artifact must be a relative portable path");

  auto leading_zero = base();
  leading_zero.replace(leading_zero.find("sequence 7"), 10, "sequence 07");
  check_error(leading_zero, neko::detail::wasm_offer_error_code::malformed_value, 3,
              "invalid wasm offer 'fixture' at line 3: sequence must be a canonical unsigned "
              "decimal integer");

  check_error(base() + "artifact \"modules/other.wasm\" \"" + std::string(a_digest) + "\"\n",
              neko::detail::wasm_offer_error_code::duplicate_field, 8,
              "invalid wasm offer 'fixture' at line 8: duplicate artifact");
}

TEST_CASE("an entry list is required") {
  auto no_entries = std::string(base());
  const auto entry_line = no_entries.rfind("entry \"update\"\n");
  no_entries.erase(entry_line);
  check_error(no_entries, neko::detail::wasm_offer_error_code::missing_field, 7,
              "invalid wasm offer 'fixture' at line 7: at least one entry is required");
}

TEST_CASE("in-memory values are validated without parsing") {
  auto value = offer();
  value.artifact_path = "../escape.wasm";
  try {
    neko::detail::validate_wasm_offer(value, "memory");
    FAIL("an escaping artifact path must be rejected");
  } catch (const neko::detail::wasm_offer_error& error) {
    CHECK(error.code() == neko::detail::wasm_offer_error_code::invalid_field);
    CHECK(std::string_view{error.what()} ==
          "invalid wasm offer 'memory' at line 0: artifact contains an invalid path component");
  }
}

TEST_CASE("supersession orders offers by sequence and identity") {
  using neko::detail::wasm_offer_ordering;
  const auto first = offer();

  CHECK(neko::detail::compare_wasm_offers(first, nullptr) == wasm_offer_ordering::supersede);

  auto second = first;
  second.sequence = 18;
  second.generation_id = "gen-18-a1";
  CHECK(neko::detail::compare_wasm_offers(first, &second) == wasm_offer_ordering::stale);
  CHECK(neko::detail::compare_wasm_offers(second, &first) == wasm_offer_ordering::supersede);
  CHECK(neko::detail::compare_wasm_offers(first, &first) == wasm_offer_ordering::duplicate);

  auto rival = second;
  rival.generation_id = "gen-18-a2";
  CHECK(neko::detail::compare_wasm_offers(rival, &second) == wasm_offer_ordering::conflict);

  auto mutated = first;
  mutated.sha256 = std::string(64, 'b');
  CHECK(neko::detail::compare_wasm_offers(mutated, &first) == wasm_offer_ordering::conflict);
}
