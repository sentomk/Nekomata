#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "protocol/group_descriptor.hpp"

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace {

neko::detail::group_descriptor descriptor() {
  neko::detail::group_descriptor value;
  value.group_id = "//gameplay:hot(//toolchain:target)";
  value.members = {"gameplay/ball", "generated/behaviour"};
  value.publication_key = "gameplay-6f10e51d";
  value.baseline_sequence = 42;
  value.compatibility_id = "sha256:compile-identity";
  value.abi_id = "elf-x86_64-patch-v1";
  value.generation_root_hint = "../reload root";
  return value;
}

void check_error(std::string_view text, neko::detail::descriptor_error_code expected_code,
                 std::size_t expected_line, std::string_view expected_message) {
  try {
    static_cast<void>(neko::detail::parse_group_descriptor(text, "fixture"));
    FAIL("descriptor must be rejected");
  } catch (const neko::detail::descriptor_error& error) {
    CHECK(error.code() == expected_code);
    CHECK(error.line() == expected_line);
    CHECK(std::string_view{error.what()} == expected_message);
  }
}

constexpr std::string_view valid_prefix = "nekomata-group/1\n"
                                          "group_id \"gameplay\"\n"
                                          "publication_key \"gameplay-a1\"\n"
                                          "baseline_sequence 7\n"
                                          "compatibility_id \"sha256:compile\"\n"
                                          "abi_id \"elf-x86_64-v1\"\n";

} // namespace

TEST_CASE("managed group descriptors have a deterministic round trip") {
  const auto original = descriptor();
  const std::string encoded = neko::detail::serialize_group_descriptor(original);

  CHECK(encoded == "nekomata-group/1\n"
                   "group_id \"//gameplay:hot(//toolchain:target)\"\n"
                   "publication_key \"gameplay-6f10e51d\"\n"
                   "baseline_sequence 42\n"
                   "compatibility_id \"sha256:compile-identity\"\n"
                   "abi_id \"elf-x86_64-patch-v1\"\n"
                   "generation_root_hint \"../reload root\"\n"
                   "member \"gameplay/ball\"\n"
                   "member \"generated/behaviour\"\n");
  CHECK(neko::detail::parse_group_descriptor(encoded, "round-trip") == original);
}

TEST_CASE("member order is descriptor identity and root hints stay lexical") {
  auto first = descriptor();
  auto second = descriptor();
  std::swap(second.members[0], second.members[1]);
  CHECK(first != second);

  first.generation_root_hint = "build/../reload";
  const auto parsed = neko::detail::parse_group_descriptor(
      neko::detail::serialize_group_descriptor(first), "lexical-root");
  REQUIRE(parsed.generation_root_hint);
  CHECK(*parsed.generation_root_hint == "build/../reload");
}

TEST_CASE("the optional generation root is omitted canonically") {
  auto value = descriptor();
  value.generation_root_hint.reset();
  const std::string encoded = neko::detail::serialize_group_descriptor(value);
  CHECK(encoded.find("generation_root_hint") == std::string::npos);
  CHECK(neko::detail::parse_group_descriptor(encoded).generation_root_hint == std::nullopt);
}

TEST_CASE("format and version failures have stable codes") {
  check_error("", neko::detail::descriptor_error_code::invalid_format, 1,
              "invalid reload group descriptor 'fixture' at line 1: missing format header");
  check_error("not-a-descriptor\n", neko::detail::descriptor_error_code::invalid_format, 1,
              "invalid reload group descriptor 'fixture' at line 1: expected "
              "nekomata-group/1");
  check_error("nekomata-group-v2\n", neko::detail::descriptor_error_code::unsupported_version, 1,
              "invalid reload group descriptor 'fixture' at line 1: expected "
              "nekomata-group/1");
  check_error("nekomata-group\n", neko::detail::descriptor_error_code::unsupported_version, 1,
              "invalid reload group descriptor 'fixture' at line 1: expected "
              "nekomata-group/1");
  check_error("nekomata-generation/2\n", neko::detail::descriptor_error_code::invalid_format, 1,
              "invalid reload group descriptor 'fixture' at line 1: expected "
              "nekomata-group/1");
}

TEST_CASE("required scalar fields occur exactly once") {
  check_error(std::string(valid_prefix) + "member \"gameplay/ball\"\n"
                                          "group_id \"again\"\n",
              neko::detail::descriptor_error_code::duplicate_field, 8,
              "invalid reload group descriptor 'fixture' at line 8: duplicate group_id");

  check_error("nekomata-group/1\n"
              "group_id \"gameplay\"\n",
              neko::detail::descriptor_error_code::missing_field, 3,
              "invalid reload group descriptor 'fixture' at line 3: missing publication_key");
}

TEST_CASE("members are nonempty ordered portable keys") {
  check_error(std::string(valid_prefix), neko::detail::descriptor_error_code::missing_field, 7,
              "invalid reload group descriptor 'fixture' at line 7: at least one member is "
              "required");
  check_error(std::string(valid_prefix) + "member \"../ball\"\n",
              neko::detail::descriptor_error_code::invalid_field, 7,
              "invalid reload group descriptor 'fixture' at line 7: member contains an "
              "invalid path component");
  check_error(std::string(valid_prefix) + "member \"gameplay\\\\ball\"\n",
              neko::detail::descriptor_error_code::invalid_field, 7,
              "invalid reload group descriptor 'fixture' at line 7: member contains a "
              "non-portable character");
  check_error(std::string(valid_prefix) + "member \"gameplay/ball\"\n"
                                          "member \"gameplay/ball\"\n",
              neko::detail::descriptor_error_code::duplicate_member, 8,
              "invalid reload group descriptor 'fixture' at line 8: duplicate member "
              "'gameplay/ball'");
}

TEST_CASE("publication keys are safe single path components") {
  auto value = descriptor();
  value.publication_key = "nested/stream";
  CHECK_THROWS_WITH_AS(neko::detail::validate_group_descriptor(value),
                       "invalid reload group descriptor '<memory>': publication_key must be one "
                       "path component",
                       neko::detail::descriptor_error);

  value.publication_key = "../stream";
  CHECK_THROWS_WITH_AS(neko::detail::validate_group_descriptor(value),
                       "invalid reload group descriptor '<memory>': publication_key must be one "
                       "path component",
                       neko::detail::descriptor_error);
}

TEST_CASE("opaque identities reject empty and control-containing values") {
  auto value = descriptor();
  value.group_id.clear();
  CHECK_THROWS_WITH_AS(neko::detail::validate_group_descriptor(value),
                       "invalid reload group descriptor '<memory>': group_id must not be empty",
                       neko::detail::descriptor_error);

  value = descriptor();
  value.compatibility_id = "compile\nidentity";
  CHECK_THROWS_WITH_AS(
      neko::detail::validate_group_descriptor(value),
      "invalid reload group descriptor '<memory>': compatibility_id must not contain control "
      "characters",
      neko::detail::descriptor_error);
}

TEST_CASE("baseline sequences require complete unsigned decimal values") {
  check_error(std::string(valid_prefix)
                      .replace(std::string(valid_prefix).find("baseline_sequence 7"),
                               std::string_view{"baseline_sequence 7"}.size(),
                               "baseline_sequence -1") +
                  "member \"gameplay/ball\"\n",
              neko::detail::descriptor_error_code::malformed_value, 4,
              "invalid reload group descriptor 'fixture' at line 4: baseline_sequence must be "
              "an unsigned decimal integer");

  const std::string overflow = std::to_string(std::numeric_limits<std::uint64_t>::max()) + "0";
  const std::string text = std::string(valid_prefix)
                               .replace(std::string(valid_prefix).find("baseline_sequence 7"),
                                        std::string_view{"baseline_sequence 7"}.size(),
                                        "baseline_sequence " + overflow) +
                           "member \"gameplay/ball\"\n";
  check_error(text, neko::detail::descriptor_error_code::malformed_value, 4,
              "invalid reload group descriptor 'fixture' at line 4: baseline_sequence must be "
              "an unsigned decimal integer");
}

TEST_CASE("syntax failures distinguish malformed trailing and unknown data") {
  check_error(std::string(valid_prefix) + "member gameplay/ball\n",
              neko::detail::descriptor_error_code::malformed_value, 7,
              "invalid reload group descriptor 'fixture' at line 7: member must be quoted");
  check_error(std::string(valid_prefix) + "member \"gameplay/ball\" trailing\n",
              neko::detail::descriptor_error_code::unexpected_value, 7,
              "invalid reload group descriptor 'fixture' at line 7: unexpected trailing fields");
  check_error(std::string(valid_prefix) + "member \"gameplay/ball\"\nextra \"value\"\n",
              neko::detail::descriptor_error_code::unknown_directive, 8,
              "invalid reload group descriptor 'fixture' at line 8: unknown directive 'extra'");
}

TEST_CASE("descriptor error-code names are stable") {
  using neko::detail::descriptor_error_code;
  CHECK(neko::detail::descriptor_error_code_name(descriptor_error_code::invalid_format) ==
        "invalid_format");
  CHECK(neko::detail::descriptor_error_code_name(descriptor_error_code::unsupported_version) ==
        "unsupported_version");
  CHECK(neko::detail::descriptor_error_code_name(descriptor_error_code::unknown_directive) ==
        "unknown_directive");
  CHECK(neko::detail::descriptor_error_code_name(descriptor_error_code::malformed_value) ==
        "malformed_value");
  CHECK(neko::detail::descriptor_error_code_name(descriptor_error_code::unexpected_value) ==
        "unexpected_value");
  CHECK(neko::detail::descriptor_error_code_name(descriptor_error_code::duplicate_field) ==
        "duplicate_field");
  CHECK(neko::detail::descriptor_error_code_name(descriptor_error_code::missing_field) ==
        "missing_field");
  CHECK(neko::detail::descriptor_error_code_name(descriptor_error_code::invalid_field) ==
        "invalid_field");
  CHECK(neko::detail::descriptor_error_code_name(descriptor_error_code::duplicate_member) ==
        "duplicate_member");
}
