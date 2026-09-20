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

TEST_CASE("cancellation discards late success and late failure") {
  lifetime life;
  test_loader loader;
  candidate pending(loader, "late.wasm", contract());
  pending.cancel();
  pending.cancel();
  SUBCASE("success") {
    loader.finish(std::make_unique<test_image>(life));
    CHECK(life.released == 1);
  }
  SUBCASE("failure") {
    loader.finish(nullptr, "late error");
  }
  CHECK(pending.status() == candidate_status::cancelled);
  CHECK(pending.error() == candidate_error::none);
  CHECK(pending.message().empty());
  active_module active;
  CHECK_FALSE(active.activate(pending));
  CHECK(life.pinned == 0);
}

TEST_CASE("destruction before completion releases late code without resurrecting the candidate") {
  lifetime life;
  module_loader::completion callback;
  {
    test_loader loader;
    candidate pending(loader, "late.wasm", contract());
    callback = std::move(loader.pending);
  }
  callback({std::make_unique<test_image>(life), {}});
  CHECK(life.released == 1);
  CHECK(life.pinned == 0);
}

TEST_CASE("moving a pending candidate transfers its completion") {
  lifetime life;
  test_loader loader;
  candidate original(loader, "late.wasm", contract());
  candidate moved(std::move(original));
  CHECK(original.status() == candidate_status::cancelled);
  original.cancel();
  loader.finish(std::make_unique<test_image>(life));
  CHECK(moved.status() == candidate_status::ready);
  active_module active;
  REQUIRE(active.activate(moved));
  moved.cancel();
  CHECK(moved.status() == candidate_status::activated);
  CHECK(life.pinned == 1);
}

TEST_CASE("move assignment discards the old request and accepts only the new one") {
  lifetime old_life, new_life;
  test_loader loader;
  candidate target(loader, "old.wasm", contract());
  auto old_callback = std::move(loader.pending);
  candidate source(loader, "new.wasm", contract());
  target = std::move(source);
  old_callback({std::make_unique<test_image>(old_life), {}});
  CHECK(target.status() == candidate_status::loading);
  CHECK(old_life.released == 1);
  loader.finish(std::make_unique<test_image>(new_life));
  CHECK(target.status() == candidate_status::ready);
  target.cancel();
  CHECK(target.status() == candidate_status::cancelled);
  CHECK(new_life.released == 1);
}

TEST_CASE("synchronous completion is safe during construction") {
  lifetime life;
  class inline_loader final : public module_loader {
  public:
    explicit inline_loader(lifetime& life) : life_(life) {}
    void open(std::string, completion complete) override {
      complete({std::make_unique<test_image>(life_), {}});
    }

  private:
    lifetime& life_;
  } loader(life);
  candidate pending(loader, "cached.wasm", contract());
  CHECK(pending.status() == candidate_status::ready);
  pending.cancel();
  CHECK(life.released == 1);
}

TEST_CASE("prepared entries own a snapshot of the descriptor metadata") {
  calls = 0;
  lifetime life;
  test_loader loader;
  candidate pending(loader, "mutable.wasm", contract());
  auto image = std::make_unique<test_image>(life);
  auto* original = image.get();
  char name[] = "tick";
  image->entries[0].name = name;
  loader.finish(std::move(image));
  original->entries[0].address = behavior_b;
  name[0] = 'x';
  active_module active;
  REQUIRE(active.activate(pending));
  REQUIRE(active.current()->entry("tick") != nullptr);
  active.current()->entry("tick")();
  CHECK(calls == 1);
  CHECK(active.current()->entry("xick") == nullptr);
}

TEST_CASE("rejected or cancelled candidates cannot displace the active module") {
  lifetime life;
  test_loader loader;
  active_module active;
  candidate first(loader, "a.wasm", contract());
  loader.finish(std::make_unique<test_image>(life));
  REQUIRE(active.activate(first));
  const auto before = active.current();
  candidate second(loader, "b.wasm", contract());
  SUBCASE("rejected") {
    loader.finish(nullptr, "broken");
  }
  SUBCASE("cancelled") {
    second.cancel();
  }
  CHECK_FALSE(active.activate(second));
  CHECK(active.current() == before);
  CHECK(life.pinned == 1);
}

TEST_CASE("incompatible headers are rejected without reading a descriptor tail") {
  class header_image final : public module_image {
  public:
    const module_header* descriptor() const noexcept override { return &header; }
    void keep_resident() noexcept override {}
    module_header header{2, sizeof(module_descriptor)};
  };
  test_loader loader;
  candidate pending(loader, "short.wasm", contract());
  auto image = std::make_unique<header_image>();
  SUBCASE("unsupported version") {}
  SUBCASE("truncated known version") {
    image->header = {1, sizeof(module_header)};
  }
  loader.finish(std::move(image));
  CHECK(pending.error() == candidate_error::incompatible);
}

} // namespace
