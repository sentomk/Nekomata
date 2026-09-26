#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <array>
#include <backends/wasm/registration.hpp>
#include <stdexcept>
#include <string>

namespace {
using namespace neko::wasm::detail;
constexpr std::array<std::string_view, 2> entries{"tick", "identity"};
} // namespace

TEST_CASE("build registrations are copied and sorted without changing their records") {
  std::string id = "alpha";
  group_record alpha{id, "alpha/latest", "test-v1", entries};
  group_record beta{"beta", "beta/latest", "test-v2", entries, &alpha};
  const auto result = read_group_records(&beta);
  REQUIRE(result.size() == 2);
  CHECK(result[0].group_id == "alpha");
  CHECK(result[1].group_id == "beta");
  CHECK(result[0].manifest_url == "alpha/latest");
  CHECK(result[0].expected_contract.abi_id == "test-v1");
  CHECK(result[1].expected_contract.entries == std::vector<std::string>{"tick", "identity"});
  CHECK(result[0].expected_contract.sha256.empty());
  CHECK(beta.next == &alpha);
  id[0] = 'x';
  CHECK(result[0].group_id == "alpha");
  CHECK(read_group_records(nullptr).empty());
}

TEST_CASE("invalid generated registration cannot reach observation") {
  group_record record{"game", "offers/latest", "test-v1", entries};
  SUBCASE("empty ID") {
    record.group_id = {};
  }
  SUBCASE("empty URL") {
    record.manifest_url = {};
  }
  SUBCASE("empty ABI") {
    record.abi_id = {};
  }
  SUBCASE("empty membership") {
    record.entries = {};
  }
  SUBCASE("embedded NUL") {
    record.abi_id = std::string_view{"ab\0i", 4};
  }
  CHECK_THROWS_WITH_AS(static_cast<void>(read_group_records(&record)),
                       "wasm registration: nonempty ID, URL, ABI and entries are required",
                       std::invalid_argument);
}

TEST_CASE("generated entry names must be nonempty and unique") {
  std::array<std::string_view, 2> names{"tick", "identity"};
  SUBCASE("empty") {
    names[1] = {};
  }
  SUBCASE("duplicate") {
    names[1] = "tick";
  }
  SUBCASE("embedded NUL") {
    names[1] = std::string_view{"ti\0ck", 5};
  }
  group_record record{"game", "offers/latest", "test-v1", names};
  CHECK_THROWS_WITH_AS(static_cast<void>(read_group_records(&record)),
                       "wasm registration: entry names must be nonempty and unique",
                       std::invalid_argument);
}

TEST_CASE("duplicate build group identities are rejected without consuming the records") {
  group_record first{"same", "first/latest", "test-v1", entries};
  group_record second{"same", "second/latest", "test-v1", entries, &first};
  CHECK_THROWS_WITH_AS(static_cast<void>(read_group_records(&second)),
                       "wasm registration: duplicate reload group 'same'", std::invalid_argument);
  CHECK(second.next == &first);
  second.group_id = "different";
  CHECK(read_group_records(&second).size() == 2);
}

TEST_CASE("startup registration is idempotent for the same static record") {
  static group_record first{"first", "first/latest", "test-v1", entries};
  static group_record second{"second", "second/latest", "test-v1", entries};
  register_group(first);
  register_group(second);
  register_group(first);
  const auto found = discover_groups();
  REQUIRE(found.size() == 2);
  CHECK(found[0].group_id == "first");
  CHECK(found[1].group_id == "second");
  CHECK(discover_groups().size() == 2);
}

TEST_CASE("PLT slots must name a registered group and one of its entries") {
  group_record game{"game", "game/latest", "test-v1", entries};
  const auto groups = read_group_records(&game);
  plt_slot_record tick{"game", "tick", nullptr, nullptr};
  plt_slot_record identity{"game", "identity", nullptr, nullptr, &tick};
  CHECK_NOTHROW(validate_plt_slots(groups, &identity));
  CHECK_NOTHROW(validate_plt_slots(groups, nullptr));

  SUBCASE("unknown group") {
    plt_slot_record foreign{"other", "tick", nullptr, nullptr, &identity};
    CHECK_THROWS_WITH_AS(validate_plt_slots(groups, &foreign),
                         "wasm registration: PLT slot group 'other' is not registered",
                         std::invalid_argument);
  }
  SUBCASE("entry outside the contract") {
    plt_slot_record ghost{"game", "ghost", nullptr, nullptr, &identity};
    CHECK_THROWS_WITH_AS(validate_plt_slots(groups, &ghost),
                         "wasm registration: PLT slot entry 'ghost' is not in group 'game'",
                         std::invalid_argument);
  }
  SUBCASE("slot without any registered group") {
    CHECK_THROWS_WITH_AS(validate_plt_slots({}, &tick),
                         "wasm registration: PLT slot group 'game' is not registered",
                         std::invalid_argument);
  }
}
