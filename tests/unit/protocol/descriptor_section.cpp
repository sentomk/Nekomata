#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "protocol/descriptor_section.hpp"

#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

neko::detail::group_descriptor descriptor_a() {
  neko::detail::group_descriptor value;
  value.group_id = "//gameplay:hot";
  value.members = {"gameplay/ball", "gameplay/gravity"};
  value.publication_key = "gameplay-6f10e51d";
  value.baseline_sequence = 42;
  value.compatibility_id = "sha256:compile-identity";
  value.abi_id = "elf-x86_64-patch-v1";
  return value;
}

neko::detail::group_descriptor descriptor_b() {
  auto value = descriptor_a();
  value.group_id = "//editor:inspector";
  value.members = {"editor/inspector"};
  value.publication_key = "editor-9c1f7a30";
  value.baseline_sequence = 7;
  return value;
}

void append_record(std::vector<std::uint8_t>& bytes, std::string_view payload) {
  const auto length = static_cast<std::uint32_t>(payload.size());
  for (int i = 0; i < 4; ++i) {
    bytes.push_back(static_cast<std::uint8_t>(length >> (i * 8)));
  }
  bytes.insert(bytes.end(), payload.begin(), payload.end());
}

} // namespace

TEST_CASE("the descriptor section framing round trips") {
  const std::vector<neko::detail::group_descriptor> descriptors{descriptor_a(), descriptor_b()};
  const auto bytes = neko::detail::serialize_descriptor_section(descriptors);

  const auto records = neko::detail::parse_descriptor_section(bytes, "section");
  REQUIRE(records.size() == 2);
  CHECK(records[0].descriptor == descriptors[0]);
  CHECK(records[1].descriptor == descriptors[1]);
  CHECK(records[0].offset == 0);

  const auto& first = neko::detail::serialize_group_descriptor(descriptors[0]);
  CHECK(records[1].offset == 4 + first.size());

  CHECK(neko::detail::serialize_descriptor_section({}) == std::vector<std::uint8_t>{});
  CHECK(neko::detail::parse_descriptor_section({}, "section").empty());
}

TEST_CASE("framing rejects truncated and empty records") {
  const auto good = neko::detail::serialize_descriptor_section({descriptor_a()});

  std::vector<std::uint8_t> short_header(good.begin(), good.begin() + 2);
  CHECK_THROWS_WITH_AS(
      static_cast<void>(neko::detail::parse_descriptor_section(short_header, "section")),
      "invalid descriptor section 'section': record at offset 0 is truncated", std::runtime_error);

  std::vector<std::uint8_t> short_payload(good.begin(), good.end() - 1);
  CHECK_THROWS_WITH_AS(
      static_cast<void>(neko::detail::parse_descriptor_section(short_payload, "section")),
      "invalid descriptor section 'section': record at offset 0 is truncated", std::runtime_error);

  std::vector<std::uint8_t> zero_padding{0, 0, 0, 0};
  CHECK(neko::detail::parse_descriptor_section(zero_padding, "section")
            .empty()); // alignment padding after no records

  std::vector<std::uint8_t> zero_length{0, 0, 0, 0, 1};
  CHECK_THROWS_WITH_AS(
      static_cast<void>(neko::detail::parse_descriptor_section(zero_length, "section")),
      "invalid descriptor section 'section': record at offset 0 has a zero length",
      std::runtime_error);

  std::vector<std::uint8_t> trailing(good);
  trailing.push_back(1);
  trailing.push_back(2);
  trailing.push_back(3);
  const auto payload = neko::detail::serialize_group_descriptor(descriptor_a());
  const std::size_t end_of_first = 4 + payload.size();
  try {
    static_cast<void>(neko::detail::parse_descriptor_section(trailing, "section"));
    FAIL("trailing nonzero bytes must be rejected");
  } catch (const std::runtime_error& error) {
    CHECK(std::string_view{error.what()} ==
          "invalid descriptor section 'section': record at offset " + std::to_string(end_of_first) +
              " is truncated");
  }
}

TEST_CASE("corrupted payloads report their record offset") {
  std::vector<std::uint8_t> bytes;
  const auto first = neko::detail::serialize_group_descriptor(descriptor_a());
  append_record(bytes, first);
  const std::size_t second_offset = bytes.size();
  append_record(bytes, "not-a-descriptor\n");

  try {
    static_cast<void>(neko::detail::parse_descriptor_section(bytes, "section"));
    FAIL("a corrupted payload must be rejected");
  } catch (const neko::detail::descriptor_error& error) {
    CHECK(std::string_view{error.what()} ==
          "invalid reload group descriptor 'section record at offset " +
              std::to_string(second_offset) + "' at line 1: expected nekomata-group-v1");
  }
}

TEST_CASE("identical duplicates collapse and conflicts are rejected") {
  std::vector<neko::detail::descriptor_record> records{
      {descriptor_a(), 0},
      {descriptor_b(), 20},
      {descriptor_a(), 60},
  };
  const auto resolved = neko::detail::resolve_descriptor_records(records);
  REQUIRE(resolved.size() == 2);
  CHECK(resolved[0] == descriptor_a());
  CHECK(resolved[1] == descriptor_b());

  auto conflicting = descriptor_a();
  conflicting.baseline_sequence += 1;
  records.push_back({conflicting, 90});
  REQUIRE_THROWS_WITH_AS(
      static_cast<void>(neko::detail::resolve_descriptor_records(records)),
      "conflicting embedded descriptors for group '//gameplay:hot' at offsets 0 and 90",
      std::runtime_error);
}
