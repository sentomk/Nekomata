#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <backends/wasm/session.hpp>

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using namespace neko::wasm;

constexpr std::string_view a_digest =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr std::string_view b_digest =
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

void behavior_a() {}
void behavior_b() {}

class test_loader final : public module_loader {
public:
  void open(std::string path, std::string_view sha256, completion complete) override {
    static_cast<void>(sha256);
    opened_paths.emplace_back(std::move(path));
    pending = std::move(complete);
  }
  void finish(std::unique_ptr<module_image> image, std::string message = {},
              module_load_status status = module_load_status::loaded) {
    auto callback = std::move(pending);
    callback({std::move(image), std::move(message), status});
  }
  std::vector<std::string> opened_paths;
  completion pending;
};

class scripted_fetcher final : public manifest_fetcher {
public:
  void fetch(std::string url, completion complete) override {
    static_cast<void>(url);
    pending = std::move(complete);
  }
  void deliver_ok(std::string text) {
    auto callback = std::move(pending);
    callback({true, std::move(text), {}});
  }
  completion pending;
};

class test_image final : public module_image {
public:
  explicit test_image(module_function function) {
    entries[0] = {"tick", function};
    entries[1] = {"identity", function};
  }
  module_entry entries[2];
  module_descriptor value{
      {module_interface_version, sizeof(module_descriptor)}, "test-v1", 2, entries};
  const module_header* descriptor() const noexcept override { return &value.header; }
  void keep_resident() noexcept override {}
};

std::string offer_text(std::uint64_t sequence, std::string_view generation, std::string_view digest,
                       std::string_view group = "game") {
  return "nekomata-wasm/1\n"
         "group_id \"" +
         std::string(group) +
         "\"\n"
         "sequence " +
         std::to_string(sequence) +
         "\n"
         "generation_id \"" +
         std::string(generation) +
         "\"\n"
         "abi_id \"test-v1\"\n"
         "artifact \"modules/gen.wasm\" \"" +
         std::string(digest) + "\"\n" + "entry \"tick\"\n" + "entry \"identity\"\n";
}

// One polling round: update() starts the manifest fetch, the scripted
// fetcher completes it, and the next update() observes the outcome.
void poll(reload_session& session, scripted_fetcher& fetcher, std::string text) {
  static_cast<void>(session.update());
  fetcher.deliver_ok(std::move(text));
}

struct diagnostics_log {
  std::vector<offer_event_kind> kinds;
  std::vector<std::string> messages;
  void record(const offer_event& event) {
    kinds.push_back(event.kind);
    messages.push_back(event.message);
  }
};

} // namespace

TEST_CASE("an empty expected group is a configuration error") {
  test_loader loader;
  scripted_fetcher fetcher;
  REQUIRE_THROWS_AS((reload_session{loader, fetcher, "offers/latest", ""}), std::runtime_error);
}

TEST_CASE("a ready generation applies at the update safe point") {
  test_loader loader;
  scripted_fetcher fetcher;
  reload_session session{loader, fetcher, "offers/latest", "game"};
  REQUIRE(session.current() == nullptr);
  CHECK(session.update().events.empty());

  poll(session, fetcher, offer_text(7, "gen-7", a_digest));

  // The offer was accepted with the manifest fetch; the candidate is still
  // loading, so nothing applies yet.
  auto loading = session.update();
  CHECK(loading.events.empty());
  CHECK(session.current() == nullptr);

  loader.finish(std::make_unique<test_image>(behavior_a));
  auto applied = session.update();
  REQUIRE(applied.events.size() == 1);
  const auto& event = applied.events.front();
  CHECK(event.status == update_status::applied);
  CHECK(event.code == candidate_error::none);
  CHECK(event.generation_id == "gen-7");
  CHECK(event.redirected_entry_count == 2);
  CHECK(event.message.empty());
  REQUIRE(session.current() != nullptr);
  CHECK(session.current()->entry_count() == 2);
  CHECK(session.current()->entry("tick") == behavior_a);

  // Reported exactly once: later updates stay quiet.
  CHECK(session.update().events.empty());
}

TEST_CASE("a rejected generation reports once and the active set survives") {
  test_loader loader;
  scripted_fetcher fetcher;
  reload_session session{loader, fetcher, "offers/latest", "game"};

  poll(session, fetcher, offer_text(7, "gen-7", a_digest));
  loader.finish(std::make_unique<test_image>(behavior_a));
  REQUIRE(session.update().events.front().status == update_status::applied);
  const auto active = session.current();
  REQUIRE(active != nullptr);

  poll(session, fetcher, offer_text(8, "gen-8", b_digest));
  static_cast<void>(session.update());
  loader.finish(nullptr, "wasm artifact digest mismatch for 'modules/gen.wasm'",
                module_load_status::digest_mismatch);

  auto rejected = session.update();
  REQUIRE(rejected.events.size() == 1);
  const auto& event = rejected.events.front();
  CHECK(event.status == update_status::rejected);
  CHECK(event.code == candidate_error::integrity);
  CHECK(event.generation_id == "gen-8");
  CHECK(event.message == "wasm artifact digest mismatch for 'modules/gen.wasm'");

  CHECK(session.current() == active);
  CHECK(session.current()->entry("tick") == behavior_a);
  CHECK(session.update().events.empty());
}

TEST_CASE("a superseding generation activates after an applied one") {
  test_loader loader;
  scripted_fetcher fetcher;
  reload_session session{loader, fetcher, "offers/latest", "game"};

  poll(session, fetcher, offer_text(7, "gen-7", a_digest));
  loader.finish(std::make_unique<test_image>(behavior_a));
  REQUIRE(session.update().events.front().status == update_status::applied);
  const auto first = session.current();

  poll(session, fetcher, offer_text(8, "gen-8", b_digest));
  loader.finish(std::make_unique<test_image>(behavior_b));
  auto second = session.update();
  REQUIRE(second.events.front().status == update_status::applied);
  CHECK(second.events.front().generation_id == "gen-8");
  CHECK(session.current() != first);
  CHECK(session.current()->entry("tick") == behavior_b);
  // The previous snapshot keeps resolving to its own generation's code.
  CHECK(first->entry("tick") == behavior_a);
}

TEST_CASE("offers for another group are diagnostics, not transactions") {
  test_loader loader;
  scripted_fetcher fetcher;
  diagnostics_log log;
  reload_session session{loader, fetcher, "offers/latest", "game",
                         [&log](const offer_event& event) { log.record(event); }};

  poll(session, fetcher, offer_text(7, "gen-7", a_digest, "other-game"));
  CHECK(session.update().events.empty());
  CHECK(session.current() == nullptr);
  CHECK(loader.opened_paths.empty());
  REQUIRE(log.kinds.size() == 1);
  CHECK(log.kinds.front() == offer_event_kind::offer_ignored);
  CHECK(log.messages.front() == "offer group 'other-game' does not match watched group 'game'");

  // The wrongly-grouped offer never became the ordering reference.
  poll(session, fetcher, offer_text(7, "gen-7", a_digest));
  CHECK(!loader.opened_paths.empty());
}

TEST_CASE("candidate replacement between updates reports only the newest") {
  test_loader loader;
  scripted_fetcher fetcher;
  reload_session session{loader, fetcher, "offers/latest", "game"};

  poll(session, fetcher, offer_text(7, "gen-7", a_digest));
  static_cast<void>(session.update());
  CHECK(loader.opened_paths.size() == 1);

  // Supersede before the first candidate ever finishes.
  poll(session, fetcher, offer_text(8, "gen-8", b_digest));
  CHECK(session.update().events.empty());
  CHECK(loader.opened_paths.size() == 2);

  loader.finish(std::make_unique<test_image>(behavior_b));
  auto applied = session.update();
  REQUIRE(applied.events.size() == 1);
  CHECK(applied.events.front().generation_id == "gen-8");
  CHECK(applied.events.front().status == update_status::applied);
}
