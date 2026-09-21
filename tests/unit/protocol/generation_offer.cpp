#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "protocol/generation_offer.hpp"
#include "protocol/group_descriptor.hpp"

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace {

constexpr std::string_view a_digest =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr std::string_view b_digest =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

neko::detail::generation_offer offer() {
  neko::detail::generation_offer value;
  value.group_id = "//gameplay:hot(//toolchain:target)";
  value.sequence = 43;
  value.generation_id = "gen-43-a1";
  value.compatibility_id = "sha256:compile-identity";
  value.abi_id = "elf-x86_64-patch-v1";
  value.members = {
      {"gameplay/ball", "objects/gameplay/ball.o", std::string(a_digest), "//src:ball.cpp",
       "clang++ -O2 (diagnostic only)"},
      {"generated/behaviour", "objects/generated/behaviour.o", std::string(b_digest),
       "//generated:behaviour.cpp", ""},
  };
  value.changed_inputs = {"//src:ball.cpp", "//include:physics constants.hpp"};
  return value;
}

neko::detail::group_descriptor descriptor() {
  neko::detail::group_descriptor value;
  value.group_id = "//gameplay:hot(//toolchain:target)";
  value.members = {"gameplay/ball", "generated/behaviour"};
  value.publication_key = "gameplay-6f10e51d";
  value.baseline_sequence = 42;
  value.compatibility_id = "sha256:compile-identity";
  value.abi_id = "elf-x86_64-patch-v1";
  return value;
}

void check_error(std::string_view text, neko::detail::generation_offer_error_code expected_code,
                 std::size_t expected_line, std::string_view expected_message) {
  try {
    static_cast<void>(neko::detail::parse_generation_offer(text, "fixture"));
    FAIL("generation offer must be rejected");
  } catch (const neko::detail::generation_offer_error& error) {
    CHECK(error.code() == expected_code);
    CHECK(error.line() == expected_line);
    CHECK(std::string_view{error.what()} == expected_message);
  }
}

constexpr std::string_view valid_prefix = "nekomata-generation 2\n"
                                          "group_id \"gameplay\"\n"
                                          "sequence 7\n"
                                          "generation_id \"gen-7\"\n"
                                          "compatibility_id \"sha256:compile\"\n"
                                          "abi_id \"elf-x86_64-v1\"\n";

std::string member_line(std::string_view member = "gameplay/ball",
                        std::string_view object_path = "objects/gameplay/ball.o",
                        std::string_view digest = a_digest) {
  return "member \"" + std::string(member) + "\" \"" + std::string(object_path) + "\" \"" +
         std::string(digest) + "\" \"//src:ball.cpp\" \"-O2\"\n";
}

} // namespace

TEST_CASE("managed generation offers have a deterministic round trip") {
  const auto original = offer();
  const std::string encoded = neko::detail::serialize_generation_offer(original);

  CHECK(encoded ==
        "nekomata-generation 2\n"
        "group_id \"//gameplay:hot(//toolchain:target)\"\n"
        "sequence 43\n"
        "generation_id \"gen-43-a1\"\n"
        "compatibility_id \"sha256:compile-identity\"\n"
        "abi_id \"elf-x86_64-patch-v1\"\n"
        "member \"gameplay/ball\" \"objects/gameplay/ball.o\" \"aaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\" \"//src:ball.cpp\" \"clang++ -O2 "
        "(diagnostic only)\"\n"
        "member \"generated/behaviour\" \"objects/generated/behaviour.o\" \"0123456789abcdef"
        "0123456789abcdef0123456789abcdef0123456789abcdef\" "
        "\"//generated:behaviour.cpp\" \"\"\n"
        "changed_input \"//src:ball.cpp\"\n"
        "changed_input \"//include:physics constants.hpp\"\n");
  CHECK(neko::detail::parse_generation_offer(encoded, "round-trip") == original);
}

TEST_CASE("blank lines and whole-line comments are accepted but not serialized") {
  const auto expected = offer();
  auto encoded = neko::detail::serialize_generation_offer(expected);
  const auto after_header = encoded.find('\n') + 1;
  encoded.insert(after_header, "\n  # producer diagnostic\n");
  CHECK(neko::detail::parse_generation_offer(encoded, "comments") == expected);
}

TEST_CASE("format and scalar failures are classified") {
  check_error("", neko::detail::generation_offer_error_code::invalid_format, 1,
              "invalid generation offer 'fixture' at line 1: missing format header");
  check_error("not-an-offer\n", neko::detail::generation_offer_error_code::invalid_format, 1,
              "invalid generation offer 'fixture' at line 1: expected nekomata-generation 2");
  check_error("nekomata-generation 3\n",
              neko::detail::generation_offer_error_code::unsupported_version, 1,
              "invalid generation offer 'fixture' at line 1: expected nekomata-generation 2");
  check_error("nekomata-generation-v2\n",
              neko::detail::generation_offer_error_code::unsupported_version, 1,
              "invalid generation offer 'fixture' at line 1: expected nekomata-generation 2");
  check_error("nekomata-generation\n",
              neko::detail::generation_offer_error_code::unsupported_version, 1,
              "invalid generation offer 'fixture' at line 1: expected nekomata-generation 2");
  check_error("nekomata-group 1\n", neko::detail::generation_offer_error_code::invalid_format, 1,
              "invalid generation offer 'fixture' at line 1: expected nekomata-generation 2");
  check_error(std::string(valid_prefix) + member_line() + "sequence 8\n",
              neko::detail::generation_offer_error_code::duplicate_field, 8,
              "invalid generation offer 'fixture' at line 8: duplicate sequence");
  check_error("nekomata-generation 2\ngroup_id \"gameplay\"\n",
              neko::detail::generation_offer_error_code::missing_field, 3,
              "invalid generation offer 'fixture' at line 3: missing sequence");
}

TEST_CASE("sequences use one canonical unsigned representation") {
  auto text = std::string(valid_prefix);
  text.replace(text.find("sequence 7"), std::string_view{"sequence 7"}.size(), "sequence 07");
  check_error(text + member_line(), neko::detail::generation_offer_error_code::malformed_value, 3,
              "invalid generation offer 'fixture' at line 3: sequence must be a canonical "
              "unsigned decimal integer");

  const std::string overflow = std::to_string(std::numeric_limits<std::uint64_t>::max()) + "0";
  text = std::string(valid_prefix);
  text.replace(text.find("sequence 7"), std::string_view{"sequence 7"}.size(),
               "sequence " + overflow);
  check_error(text + member_line(), neko::detail::generation_offer_error_code::malformed_value, 3,
              "invalid generation offer 'fixture' at line 3: sequence must be a canonical "
              "unsigned decimal integer");
}

TEST_CASE("member records require safe unique keys paths and digests") {
  check_error(std::string(valid_prefix), neko::detail::generation_offer_error_code::missing_field,
              7, "invalid generation offer 'fixture' at line 7: at least one member is required");
  check_error(std::string(valid_prefix) + member_line("../ball"),
              neko::detail::generation_offer_error_code::invalid_field, 7,
              "invalid generation offer 'fixture' at line 7: member contains an invalid path "
              "component");
  check_error(std::string(valid_prefix) + member_line("gameplay/ball", "../ball.o"),
              neko::detail::generation_offer_error_code::invalid_field, 7,
              "invalid generation offer 'fixture' at line 7: object_path contains an invalid "
              "path component");
  check_error(std::string(valid_prefix) + member_line("gameplay/ball", "cache/ball.o"),
              neko::detail::generation_offer_error_code::invalid_field, 7,
              "invalid generation offer 'fixture' at line 7: object_path must be below objects/");
  check_error(std::string(valid_prefix) + member_line("gameplay/ball", "objects/ball.o", b_digest) +
                  member_line("gameplay/ball", "objects/again.o", a_digest),
              neko::detail::generation_offer_error_code::duplicate_member, 8,
              "invalid generation offer 'fixture' at line 8: duplicate member 'gameplay/ball'");
  check_error(std::string(valid_prefix) + member_line("gameplay/ball", "objects/shared.o") +
                  member_line("gameplay/gravity", "objects/shared.o", b_digest),
              neko::detail::generation_offer_error_code::duplicate_object_path, 8,
              "invalid generation offer 'fixture' at line 8: duplicate object_path "
              "'objects/shared.o'");
  check_error(std::string(valid_prefix) +
                  member_line("gameplay/ball", "objects/ball.o",
                              "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"),
              neko::detail::generation_offer_error_code::invalid_field, 7,
              "invalid generation offer 'fixture' at line 7: sha256 must contain exactly 64 "
              "lowercase hexadecimal digits");
}

TEST_CASE("changed inputs are optional ordered diagnostics but cannot repeat") {
  const std::string text = std::string(valid_prefix) + member_line() +
                           "changed_input \"//src:ball.cpp\"\n"
                           "changed_input \"//src:ball.cpp\"\n";
  check_error(text, neko::detail::generation_offer_error_code::duplicate_changed_input, 9,
              "invalid generation offer 'fixture' at line 9: duplicate changed_input "
              "'//src:ball.cpp'");

  auto value = offer();
  value.changed_inputs.clear();
  CHECK(neko::detail::parse_generation_offer(neko::detail::serialize_generation_offer(value)) ==
        value);
}

TEST_CASE("syntax rejects missing quotes trailing values and unknown directives") {
  check_error(std::string(valid_prefix) + "member gameplay/ball \"objects/ball.o\" \"" +
                  std::string(a_digest) + "\" \"//src:ball.cpp\" \"-O2\"\n",
              neko::detail::generation_offer_error_code::malformed_value, 7,
              "invalid generation offer 'fixture' at line 7: member must be quoted");
  check_error(std::string(valid_prefix) + member_line() + "changed_input \"a\" trailing\n",
              neko::detail::generation_offer_error_code::unexpected_value, 8,
              "invalid generation offer 'fixture' at line 8: unexpected trailing fields");
  check_error(std::string(valid_prefix) + member_line() + "extra \"value\"\n",
              neko::detail::generation_offer_error_code::unknown_directive, 8,
              "invalid generation offer 'fixture' at line 8: unknown directive 'extra'");
}

TEST_CASE("ready marker names round trip and match their manifest") {
  const neko::detail::generation_offer_reference reference{43, "gen-43-a1"};
  CHECK(neko::detail::serialize_generation_offer_marker(reference) == "43-gen-43-a1.ready");
  CHECK(neko::detail::parse_generation_offer_marker("43-gen-43-a1.ready", "marker") == reference);
  CHECK_NOTHROW(neko::detail::validate_generation_offer_reference(reference, offer(), "marker"));

  CHECK_THROWS_WITH_AS(
      static_cast<void>(
          neko::detail::parse_generation_offer_marker("043-gen-43-a1.ready", "marker")),
      "invalid generation offer 'marker': sequence must be a canonical unsigned decimal integer",
      neko::detail::generation_offer_error);
  CHECK_THROWS_WITH_AS(
      static_cast<void>(neko::detail::parse_generation_offer_marker("43.ready", "marker")),
      "invalid generation offer 'marker': marker must be <sequence>-<generation_id>.ready",
      neko::detail::generation_offer_error);

  auto wrong_sequence = reference;
  wrong_sequence.sequence = 44;
  CHECK_THROWS_WITH_AS(
      neko::detail::validate_generation_offer_reference(wrong_sequence, offer(), "marker"),
      "invalid generation offer 'marker': marker sequence 44 does not match "
      "manifest sequence 43",
      neko::detail::generation_offer_error);
}

TEST_CASE("descriptor matching covers identity ABI and ordered exact membership") {
  CHECK_NOTHROW(
      neko::detail::validate_generation_offer_against_descriptor(offer(), descriptor(), "fixture"));

  auto value = offer();
  value.group_id = "other";
  CHECK_THROWS_WITH_AS(
      neko::detail::validate_generation_offer_against_descriptor(value, descriptor(), "fixture"),
      "invalid generation offer 'fixture': group_id 'other' does not match descriptor "
      "'//gameplay:hot(//toolchain:target)'",
      neko::detail::generation_offer_error);

  value = offer();
  value.compatibility_id = "different";
  CHECK_THROWS_WITH_AS(
      neko::detail::validate_generation_offer_against_descriptor(value, descriptor(), "fixture"),
      "invalid generation offer 'fixture': compatibility_id does not match descriptor",
      neko::detail::generation_offer_error);

  value = offer();
  value.abi_id = "different";
  CHECK_THROWS_WITH_AS(
      neko::detail::validate_generation_offer_against_descriptor(value, descriptor(), "fixture"),
      "invalid generation offer 'fixture': abi_id does not match descriptor",
      neko::detail::generation_offer_error);

  value = offer();
  std::swap(value.members[0], value.members[1]);
  CHECK_THROWS_WITH_AS(
      neko::detail::validate_generation_offer_against_descriptor(value, descriptor(), "fixture"),
      "invalid generation offer 'fixture': member at index 0 is 'generated/behaviour', expected "
      "'gameplay/ball'",
      neko::detail::generation_offer_error);

  value = offer();
  value.members.pop_back();
  CHECK_THROWS_WITH_AS(
      neko::detail::validate_generation_offer_against_descriptor(value, descriptor(), "fixture"),
      "invalid generation offer 'fixture': member count 1 does not match descriptor count 2",
      neko::detail::generation_offer_error);
}

TEST_CASE("offer error-code names are stable") {
  using neko::detail::generation_offer_error_code;
  CHECK(neko::detail::generation_offer_error_code_name(
            generation_offer_error_code::invalid_format) == "invalid_format");
  CHECK(neko::detail::generation_offer_error_code_name(
            generation_offer_error_code::unsupported_version) == "unsupported_version");
  CHECK(neko::detail::generation_offer_error_code_name(
            generation_offer_error_code::unknown_directive) == "unknown_directive");
  CHECK(neko::detail::generation_offer_error_code_name(
            generation_offer_error_code::malformed_value) == "malformed_value");
  CHECK(neko::detail::generation_offer_error_code_name(
            generation_offer_error_code::unexpected_value) == "unexpected_value");
  CHECK(neko::detail::generation_offer_error_code_name(
            generation_offer_error_code::duplicate_field) == "duplicate_field");
  CHECK(neko::detail::generation_offer_error_code_name(
            generation_offer_error_code::missing_field) == "missing_field");
  CHECK(neko::detail::generation_offer_error_code_name(
            generation_offer_error_code::invalid_field) == "invalid_field");
  CHECK(neko::detail::generation_offer_error_code_name(
            generation_offer_error_code::duplicate_member) == "duplicate_member");
  CHECK(neko::detail::generation_offer_error_code_name(
            generation_offer_error_code::duplicate_object_path) == "duplicate_object_path");
  CHECK(neko::detail::generation_offer_error_code_name(
            generation_offer_error_code::duplicate_changed_input) == "duplicate_changed_input");
  CHECK(neko::detail::generation_offer_error_code_name(
            generation_offer_error_code::invalid_marker) == "invalid_marker");
  CHECK(neko::detail::generation_offer_error_code_name(
            generation_offer_error_code::marker_mismatch) == "marker_mismatch");
  CHECK(neko::detail::generation_offer_error_code_name(
            generation_offer_error_code::group_mismatch) == "group_mismatch");
  CHECK(neko::detail::generation_offer_error_code_name(
            generation_offer_error_code::compatibility_mismatch) == "compatibility_mismatch");
  CHECK(neko::detail::generation_offer_error_code_name(generation_offer_error_code::abi_mismatch) ==
        "abi_mismatch");
  CHECK(neko::detail::generation_offer_error_code_name(
            generation_offer_error_code::membership_mismatch) == "membership_mismatch");
}
