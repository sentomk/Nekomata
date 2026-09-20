#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <backends/wasm/candidate.hpp>

#include <utility>

namespace {
using namespace neko::wasm;

int calls = 0;
void behavior_a() {
  calls += 1;
}
void behavior_b() {
  calls += 10;
}

struct lifetime {
  int released = 0;
  int pinned = 0;
};

class test_image final : public module_image {
public:
  explicit test_image(lifetime& life, module_function function = behavior_a) : life_(life) {
    entries[0] = {"tick", function};
  }
  ~test_image() override {
    if (!resident_) {
      ++life_.released;
    }
  }
  const module_header* descriptor() const noexcept override {
    return present ? &value.header : nullptr;
  }
  void keep_resident() noexcept override {
    resident_ = true;
    ++life_.pinned;
  }
  module_entry entries[1];
  module_descriptor value{
      {module_interface_version, sizeof(module_descriptor)}, "test-v1", 1, entries};
  bool present = true;

private:
  lifetime& life_;
  bool resident_ = false;
};

class test_loader final : public module_loader {
public:
  void open(std::string path, completion complete) override {
    opened_path = std::move(path);
    pending = std::move(complete);
  }
  void finish(std::unique_ptr<module_image> image, std::string message = {}) {
    auto callback = std::move(pending);
    callback({std::move(image), std::move(message)});
  }
  std::string opened_path;
  completion pending;
};

module_contract contract() {
  return {"test-v1", {"tick"}};
}

TEST_CASE("preparation owns code without executing it; activation consumes a ready candidate") {
  calls = 0;
  lifetime life;
  test_loader loader;
  active_module active;
  candidate pending(loader, "a.wasm", contract());
  CHECK(pending.status() == candidate_status::loading);
  CHECK_FALSE(active.activate(pending));
  CHECK_FALSE(active.current());
  loader.finish(std::make_unique<test_image>(life));
  CHECK(pending.status() == candidate_status::ready);
  CHECK(calls == 0);
  REQUIRE(active.activate(pending));
  CHECK(pending.status() == candidate_status::activated);
  CHECK_FALSE(active.activate(pending));
  CHECK(life.pinned == 1);
  CHECK(calls == 0);
  active.current()->entry("tick")();
  CHECK(calls == 1);
  CHECK(active.current()->entry("unknown") == nullptr);
}

TEST_CASE("activation replaces the whole module and old entries remain usable") {
  calls = 0;
  lifetime life_a, life_b;
  test_loader loader;
  active_module active;
  candidate a(loader, "a.wasm", contract());
  loader.finish(std::make_unique<test_image>(life_a));
  REQUIRE(active.activate(a));
  const auto old = active.current();
  candidate b(loader, "b.wasm", contract());
  loader.finish(std::make_unique<test_image>(life_b, behavior_b));
  CHECK(active.current() == old);
  REQUIRE(active.activate(b));
  active.current()->entry("tick")();
  old->entry("tick")();
  CHECK(calls == 11);
  active = active_module{};
  CHECK(life_a.released == 0);
  CHECK(life_b.released == 0);
}

TEST_CASE("invalid descriptors reject and release their loader reference") {
  lifetime life;
  test_loader loader;
  candidate pending(loader, "bad.wasm", contract());
  auto image = std::make_unique<test_image>(life);
  candidate_error expected = candidate_error::invalid_descriptor;
  std::string_view message = "invalid wasm entry descriptor";
  SUBCASE("missing descriptor") {
    image->present = false;
    expected = candidate_error::missing_descriptor;
    message = "missing wasm module descriptor";
  }
  SUBCASE("version") {
    image->value.header.version = 2;
    expected = candidate_error::incompatible;
    message = "incompatible wasm descriptor layout";
  }
  SUBCASE("size") {
    image->value.header.size = sizeof(module_header);
    expected = candidate_error::incompatible;
    message = "incompatible wasm descriptor layout";
  }
  SUBCASE("ABI") {
    image->value.abi_id = "different";
    expected = candidate_error::incompatible;
    message = "wasm module ABI mismatch";
  }
  SUBCASE("null ABI") {
    image->value.abi_id = nullptr;
    expected = candidate_error::incompatible;
    message = "wasm module ABI mismatch";
  }
  SUBCASE("count") {
    image->value.entry_count = 2;
    message = "wasm entry membership mismatch";
  }
  SUBCASE("missing table") {
    image->value.entries = nullptr;
    message = "wasm entry membership mismatch";
  }
  SUBCASE("null function") {
    image->entries[0].address = nullptr;
  }
  SUBCASE("null name") {
    image->entries[0].name = nullptr;
  }
  SUBCASE("wrong name") {
    image->entries[0].name = "other";
  }
  loader.finish(std::move(image));
  CHECK(pending.status() == candidate_status::rejected);
  CHECK(pending.error() == expected);
  CHECK(pending.message() == message);
  CHECK(life.released == 1);
  CHECK(life.pinned == 0);
  active_module active;
  CHECK_FALSE(active.activate(pending));
}

TEST_CASE("load failures carry a diagnostic") {
  test_loader loader;
  candidate pending(loader, "missing.wasm", contract());
  loader.finish(nullptr, "download failed");
  CHECK(pending.status() == candidate_status::rejected);
  CHECK(pending.error() == candidate_error::load_failed);
  CHECK(pending.message() == "download failed");
}

TEST_CASE("invalid contracts do not start I/O") {
  test_loader loader;
  auto expected = contract();
  SUBCASE("empty ABI") {
    expected.abi_id.clear();
  }
  SUBCASE("empty membership") {
    expected.entries.clear();
  }
  SUBCASE("empty name") {
    expected.entries[0].clear();
  }
  SUBCASE("duplicate name") {
    expected.entries.push_back("tick");
  }
  candidate pending(loader, "a.wasm", std::move(expected));
  CHECK(pending.status() == candidate_status::rejected);
  CHECK(pending.error() == candidate_error::invalid_contract);
  CHECK_FALSE(loader.pending);
}

TEST_CASE("ready candidate destruction releases its uncommitted module") {
  lifetime life;
  test_loader loader;
  {
    candidate pending(loader, "a.wasm", contract());
    loader.finish(std::make_unique<test_image>(life));
  }
  CHECK(life.released == 1);
  CHECK(life.pinned == 0);
}

} // namespace
