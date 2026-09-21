#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <backends/wasm/session.hpp>
#include <backends/wasm/session_error.hpp>

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
using neko::reload_error_code;
using neko::update_status;
using namespace neko::wasm;

static_assert(std::is_same_v<decltype(std::declval<reload_session&>().update()),
                             decltype(std::declval<neko::reload_session&>().update())>);

constexpr std::string_view a_digest =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr std::string_view b_digest =
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

void behavior_a() {}
void behavior_b() {}

class manual_scheduler final : public poll_scheduler {
public:
  struct task {
    std::function<void()> callback;
    bool active = true;
  };
  class subscription final : public poll_subscription {
  public:
    explicit subscription(std::shared_ptr<task> value) : task_(std::move(value)) {}
    ~subscription() override { task_->active = false; }

  private:
    std::shared_ptr<task> task_;
  };
  std::unique_ptr<poll_subscription> repeat(std::function<void()> callback) override {
    if (fail) {
      throw std::runtime_error("fixture scheduler failed");
    }
    auto value = std::make_shared<task>(task{std::move(callback)});
    tasks.push_back(value);
    return std::make_unique<subscription>(std::move(value));
  }
  void tick() {
    for (const auto& value : tasks) {
      if (value->active) {
        value->callback();
      }
    }
  }
  std::vector<std::shared_ptr<task>> tasks;
  bool fail = false;
};

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
    ++fetches;
    pending = std::move(complete);
  }
  void deliver_ok(std::string text) {
    auto callback = std::move(pending);
    callback({true, std::move(text), {}});
  }
  completion pending;
  unsigned fetches = 0;
};

class test_image final : public module_image {
public:
  explicit test_image(module_function function, bool* released = nullptr) : released_(released) {
    entries[0] = {"tick", function};
    entries[1] = {"identity", function};
  }
  module_entry entries[2];
  module_descriptor value{
      {module_interface_version, sizeof(module_descriptor)}, "test-v1", 2, entries};
  bool has_descriptor = true;
  const module_header* descriptor() const noexcept override {
    return has_descriptor ? &value.header : nullptr;
  }
  void keep_resident() noexcept override {}
  ~test_image() override {
    if (released_ != nullptr) {
      *released_ = true;
    }
  }

private:
  bool* released_;
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

// Preparation is driven by observation ticks, independently of update().
void poll(manual_scheduler& scheduler, scripted_fetcher& fetcher, std::string text) {
  scheduler.tick();
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

TEST_CASE("candidate error classification uses the public rejection vocabulary") {
  struct mapping {
    candidate_error internal;
    reload_error_code external;
  };
  constexpr mapping cases[] = {
      {candidate_error::none, reload_error_code::none},
      {candidate_error::invalid_contract, reload_error_code::invalid_artifact},
      {candidate_error::load_failed, reload_error_code::object_rejected},
      {candidate_error::integrity, reload_error_code::integrity},
      {candidate_error::missing_descriptor, reload_error_code::object_rejected},
      {candidate_error::incompatible, reload_error_code::incompatible},
      {candidate_error::invalid_descriptor, reload_error_code::object_rejected},
  };
  for (const auto& entry : cases) {
    CAPTURE(entry.internal);
    CHECK(classify_candidate_error(entry.internal) == entry.external);
    CHECK(classify_candidate_error(entry.internal) != reload_error_code::commit_failed);
  }
}

TEST_CASE("session rejection preserves identity, diagnostics and the previous entry set") {
  test_loader loader;
  scripted_fetcher fetcher;
  manual_scheduler scheduler;
  reload_session session{loader, fetcher, scheduler, "offers/latest", "physics"};
  session.watch();
  poll(scheduler, fetcher, offer_text(7, "gen-7", a_digest, "physics"));
  loader.finish(std::make_unique<test_image>(behavior_a));
  REQUIRE(session.update().any_applied());
  const auto active = session.current();

  auto image = std::make_unique<test_image>(behavior_b);
  auto status = module_load_status::loaded;
  auto expected_code = reload_error_code::object_rejected;
  std::string diagnostic;
  std::string expected_message;
  SUBCASE("load failure is not classified from diagnostic words") {
    image.reset();
    status = module_load_status::load_failed;
    diagnostic = "digest mismatch: this is only loader diagnostic text";
    expected_message = diagnostic;
  }
  SUBCASE("digest mismatch does not require a diagnostic keyword") {
    image.reset();
    status = module_load_status::digest_mismatch;
    diagnostic = "opaque loader diagnostic";
    expected_code = reload_error_code::integrity;
    expected_message = diagnostic;
  }
  SUBCASE("missing descriptor") {
    image->has_descriptor = false;
    expected_message = "missing wasm module descriptor";
  }
  SUBCASE("incompatible descriptor layout") {
    ++image->value.header.version;
    expected_code = reload_error_code::incompatible;
    expected_message = "incompatible wasm descriptor layout";
  }
  SUBCASE("incompatible application ABI") {
    image->value.abi_id = "other-abi";
    expected_code = reload_error_code::incompatible;
    expected_message = "wasm module ABI mismatch";
  }
  SUBCASE("incomplete entry set") {
    image->value.entry_count = 1;
    expected_message = "wasm entry membership mismatch";
  }
  SUBCASE("invalid entry address") {
    image->entries[0].address = nullptr;
    expected_message = "invalid wasm entry descriptor";
  }

  poll(scheduler, fetcher, offer_text(8, "gen-8", b_digest, "physics"));
  loader.finish(std::move(image), std::move(diagnostic), status);
  const neko::update_result result = session.update();
  REQUIRE(result.events.size() == 1);
  CHECK_FALSE(result.any_applied());
  const auto& event = result.events.front();
  CHECK(event.status == update_status::rejected);
  CHECK(event.code == expected_code);
  CHECK(event.group_id == "physics");
  CHECK(event.generation_id == "gen-8");
  CHECK(event.redirected_function_count == 0);
  CHECK(event.message == expected_message);
  CHECK(session.current() == active);
  CHECK(session.current()->entry("tick") == behavior_a);
  CHECK(session.update().events.empty());
}

TEST_CASE("an empty expected group is a configuration error") {
  test_loader loader;
  scripted_fetcher fetcher;
  manual_scheduler scheduler;
  REQUIRE_THROWS_AS((reload_session{loader, fetcher, scheduler, "offers/latest", ""}),
                    std::runtime_error);
}

TEST_CASE("a ready generation applies at the update safe point") {
  test_loader loader;
  scripted_fetcher fetcher;
  manual_scheduler scheduler;
  reload_session session{loader, fetcher, scheduler, "offers/latest", "game"};
  session.watch();
  REQUIRE(session.current() == nullptr);
  CHECK(session.update().events.empty());

  poll(scheduler, fetcher, offer_text(7, "gen-7", a_digest));

  // The offer was accepted with the manifest fetch; the candidate is still
  // loading, so nothing applies yet.
  auto loading = session.update();
  CHECK(loading.events.empty());
  CHECK_FALSE(loading.any_applied());
  CHECK(session.current() == nullptr);

  loader.finish(std::make_unique<test_image>(behavior_a));
  auto applied = session.update();
  REQUIRE(applied.events.size() == 1);
  CHECK(applied.any_applied());
  const auto& event = applied.events.front();
  CHECK(event.status == update_status::applied);
  CHECK(event.code == reload_error_code::none);
  CHECK(event.group_id == "game");
  CHECK(event.generation_id == "gen-7");
  CHECK(event.redirected_function_count == 2);
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
  manual_scheduler scheduler;
  reload_session session{loader, fetcher, scheduler, "offers/latest", "game"};
  session.watch();

  poll(scheduler, fetcher, offer_text(7, "gen-7", a_digest));
  loader.finish(std::make_unique<test_image>(behavior_a));
  REQUIRE(session.update().events.front().status == update_status::applied);
  const auto active = session.current();
  REQUIRE(active != nullptr);

  poll(scheduler, fetcher, offer_text(8, "gen-8", b_digest));
  static_cast<void>(session.update());
  loader.finish(nullptr, "wasm artifact digest mismatch for 'modules/gen.wasm'",
                module_load_status::digest_mismatch);

  auto rejected = session.update();
  REQUIRE(rejected.events.size() == 1);
  CHECK_FALSE(rejected.any_applied());
  const auto& event = rejected.events.front();
  CHECK(event.status == update_status::rejected);
  CHECK(event.code == reload_error_code::integrity);
  CHECK(event.group_id == "game");
  CHECK(event.redirected_function_count == 0);
  CHECK(event.generation_id == "gen-8");
  CHECK(event.message == "wasm artifact digest mismatch for 'modules/gen.wasm'");

  CHECK(session.current() == active);
  CHECK(session.current()->entry("tick") == behavior_a);
  CHECK(session.update().events.empty());
}

TEST_CASE("a superseding generation activates after an applied one") {
  test_loader loader;
  scripted_fetcher fetcher;
  manual_scheduler scheduler;
  reload_session session{loader, fetcher, scheduler, "offers/latest", "game"};
  session.watch();

  poll(scheduler, fetcher, offer_text(7, "gen-7", a_digest));
  loader.finish(std::make_unique<test_image>(behavior_a));
  REQUIRE(session.update().events.front().status == update_status::applied);
  const auto first = session.current();

  poll(scheduler, fetcher, offer_text(8, "gen-8", b_digest));
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
  manual_scheduler scheduler;
  diagnostics_log log;
  reload_session session{loader,    fetcher,
                         scheduler, "offers/latest",
                         "game",    [&log](const offer_event& event) { log.record(event); }};
  session.watch();

  poll(scheduler, fetcher, offer_text(7, "gen-7", a_digest, "other-game"));
  CHECK(session.update().events.empty());
  CHECK(session.current() == nullptr);
  CHECK(loader.opened_paths.empty());
  REQUIRE(log.kinds.size() == 1);
  CHECK(log.kinds.front() == offer_event_kind::offer_ignored);
  CHECK(log.messages.front() == "offer group 'other-game' does not match watched group 'game'");

  // The wrongly-grouped offer never became the ordering reference.
  poll(scheduler, fetcher, offer_text(7, "gen-7", a_digest));
  CHECK(!loader.opened_paths.empty());
}

TEST_CASE("candidate replacement between updates reports only the newest") {
  test_loader loader;
  scripted_fetcher fetcher;
  manual_scheduler scheduler;
  reload_session session{loader, fetcher, scheduler, "offers/latest", "game"};
  session.watch();

  poll(scheduler, fetcher, offer_text(7, "gen-7", a_digest));
  static_cast<void>(session.update());
  CHECK(loader.opened_paths.size() == 1);

  // Supersede before the first candidate ever finishes.
  poll(scheduler, fetcher, offer_text(8, "gen-8", b_digest));
  CHECK(session.update().events.empty());
  CHECK(loader.opened_paths.size() == 2);

  loader.finish(std::make_unique<test_image>(behavior_b));
  auto applied = session.update();
  REQUIRE(applied.events.size() == 1);
  CHECK(applied.events.front().generation_id == "gen-8");
  CHECK(applied.events.front().status == update_status::applied);
}

TEST_CASE("watch controls observation independently of update") {
  test_loader loader;
  scripted_fetcher fetcher;
  manual_scheduler scheduler;
  reload_session session{loader, fetcher, scheduler, "offers/latest", "game"};
  CHECK(session.update().events.empty());
  scheduler.tick();
  CHECK(scheduler.tasks.empty());
  CHECK(fetcher.fetches == 0);
  session.unwatch();
  session.unwatch("game");
  session.watch("game");
  session.watch();
  REQUIRE(scheduler.tasks.size() == 1);
  CHECK(session.update().events.empty());
  CHECK(fetcher.fetches == 0);
  poll(scheduler, fetcher, offer_text(7, "gen-7", a_digest));
  loader.finish(std::make_unique<test_image>(behavior_a));
  CHECK(session.current() == nullptr);
  REQUIRE(session.update().events.size() == 1);
  CHECK(session.current()->entry("tick") == behavior_a);
  CHECK(fetcher.fetches == 1); // Commit never starts another fetch.
}

TEST_CASE("unknown watch and unwatch groups leave the subscription unchanged") {
  test_loader loader;
  scripted_fetcher fetcher;
  manual_scheduler scheduler;
  reload_session session{loader, fetcher, scheduler, "offers/latest", "game"};
  REQUIRE_THROWS_WITH_AS(session.watch("missing"), "reload_session: unknown reload group 'missing'",
                         std::runtime_error);
  CHECK(scheduler.tasks.empty());
  session.watch();
  REQUIRE_THROWS_WITH_AS(session.unwatch("missing"),
                         "reload_session: unknown reload group 'missing'", std::runtime_error);
  REQUIRE(scheduler.tasks.size() == 1);
  CHECK(scheduler.tasks[0]->active);
  scheduler.tick();
  CHECK(fetcher.fetches == 1);
}

TEST_CASE("a paused ready candidate survives until a resumed update") {
  test_loader loader;
  scripted_fetcher fetcher;
  manual_scheduler scheduler;
  reload_session session{loader, fetcher, scheduler, "offers/latest", "game"};
  session.watch();
  poll(scheduler, fetcher, offer_text(7, "gen-7", a_digest));
  loader.finish(std::make_unique<test_image>(behavior_a));
  REQUIRE(session.update().events.size() == 1);
  const auto active = session.current();
  poll(scheduler, fetcher, offer_text(8, "gen-8", b_digest));
  loader.finish(std::make_unique<test_image>(behavior_b));
  SUBCASE("all registered groups") {
    session.unwatch();
  }
  SUBCASE("named group") {
    session.unwatch("game");
  }
  session.unwatch();
  scheduler.tick();
  CHECK(session.update().events.empty());
  CHECK(session.current() == active);
  CHECK(fetcher.fetches == 2);
  session.watch("game");
  CHECK(session.current() == active);
  const auto applied = session.update();
  REQUIRE(applied.events.size() == 1);
  CHECK(applied.events[0].generation_id == "gen-8");
  CHECK(applied.events[0].status == update_status::applied);
  CHECK(session.current()->entry("tick") == behavior_b);
  CHECK(loader.opened_paths.size() == 2);
  CHECK(session.update().events.empty());

  // Resume keeps the offer ordering reference; a repeated offer is not loaded.
  poll(scheduler, fetcher, offer_text(8, "gen-8", b_digest));
  CHECK(loader.opened_paths.size() == 2);
  CHECK(session.update().events.empty());
  poll(scheduler, fetcher, offer_text(7, "gen-7", a_digest));
  CHECK(loader.opened_paths.size() == 2);
}

TEST_CASE("manifest and artifact completions may finish while paused without activation") {
  test_loader loader;
  scripted_fetcher fetcher;
  manual_scheduler scheduler;
  reload_session session{loader, fetcher, scheduler, "offers/latest", "game"};
  session.watch();
  scheduler.tick();
  REQUIRE(fetcher.fetches == 1);
  session.unwatch();
  fetcher.deliver_ok(offer_text(7, "gen-7", a_digest));
  REQUIRE(loader.opened_paths.size() == 1);
  bool reject = false;
  SUBCASE("successful completion") {
    loader.finish(std::make_unique<test_image>(behavior_a));
  }
  SUBCASE("rejected completion") {
    reject = true;
    loader.finish(nullptr, "bad digest", module_load_status::digest_mismatch);
  }
  scheduler.tick();
  CHECK(fetcher.fetches == 1);
  CHECK(session.current() == nullptr);
  CHECK(session.update().events.empty());
  session.watch();
  const auto result = session.update();
  REQUIRE(result.events.size() == 1);
  CHECK(result.events[0].status == (reject ? update_status::rejected : update_status::applied));
  CHECK(result.events[0].code == (reject ? reload_error_code::integrity : reload_error_code::none));
  CHECK(result.events[0].group_id == "game");
  CHECK(result.events[0].redirected_function_count == (reject ? 0 : 2));
  CHECK(result.any_applied() == !reject);
  CHECK(result.events[0].generation_id == "gen-7");
  CHECK(result.events[0].message == (reject ? "bad digest" : ""));
  CHECK(session.update().events.empty());
  CHECK(loader.opened_paths.size() == 1);
}

TEST_CASE("old observation ticks stay invalid after unwatch and rewatch") {
  test_loader loader;
  scripted_fetcher fetcher;
  manual_scheduler scheduler;
  reload_session session{loader, fetcher, scheduler, "offers/latest", "game"};
  session.watch();
  const auto queued = scheduler.tasks[0]->callback;
  session.unwatch();
  queued();
  CHECK(fetcher.fetches == 0);
  session.watch();
  queued();
  CHECK(fetcher.fetches == 0);
  scheduler.tick();
  CHECK(fetcher.fetches == 1);
  session.unwatch();
  session.watch();
  scheduler.tick();
  CHECK(fetcher.fetches == 1); // The original fetch is still outstanding.
  fetcher.deliver_ok(offer_text(7, "gen-7", a_digest));
  loader.finish(std::make_unique<test_image>(behavior_a));
  REQUIRE(session.update().events.size() == 1);
}

TEST_CASE("destruction invalidates scheduled ticks and outstanding manifest callbacks") {
  test_loader loader;
  scripted_fetcher fetcher;
  manual_scheduler scheduler;
  {
    reload_session session{loader, fetcher, scheduler, "offers/latest", "game"};
    session.watch();
    scheduler.tick();
  }
  REQUIRE(scheduler.tasks.size() == 1);
  CHECK_FALSE(scheduler.tasks[0]->active);
  scheduler.tasks[0]->callback();
  CHECK(fetcher.fetches == 1);
  fetcher.deliver_ok(offer_text(7, "gen-7", a_digest));
  CHECK(loader.opened_paths.empty());
}

TEST_CASE("failed scheduling leaves watch retryable") {
  test_loader loader;
  scripted_fetcher fetcher;
  manual_scheduler scheduler;
  reload_session session{loader, fetcher, scheduler, "offers/latest", "game"};
  scheduler.fail = true;
  REQUIRE_THROWS_WITH_AS(session.watch(), "fixture scheduler failed", std::runtime_error);
  CHECK(session.update().events.empty());
  CHECK(fetcher.fetches == 0);
  scheduler.fail = false;
  session.watch();
  scheduler.tick();
  CHECK(fetcher.fetches == 1);
}

TEST_CASE("an already rejected candidate remains reportable after pause") {
  test_loader loader;
  scripted_fetcher fetcher;
  manual_scheduler scheduler;
  reload_session session{loader, fetcher, scheduler, "offers/latest", "game"};
  session.watch();
  poll(scheduler, fetcher, offer_text(7, "gen-7", a_digest));
  loader.finish(nullptr, "bad digest", module_load_status::digest_mismatch);
  session.unwatch();
  CHECK(session.update().events.empty());
  session.watch();
  const auto rejected = session.update();
  REQUIRE(rejected.events.size() == 1);
  CHECK(rejected.events[0].status == update_status::rejected);
  CHECK(rejected.events[0].code == reload_error_code::integrity);
  CHECK(rejected.events[0].group_id == "game");
  CHECK(rejected.events[0].message == "bad digest");
  CHECK(session.update().events.empty());
  CHECK(loader.opened_paths.size() == 1);
}

TEST_CASE("destruction discards an outstanding artifact completion") {
  test_loader loader;
  scripted_fetcher fetcher;
  manual_scheduler scheduler;
  {
    reload_session session{loader, fetcher, scheduler, "offers/latest", "game"};
    session.watch();
    poll(scheduler, fetcher, offer_text(7, "gen-7", a_digest));
  }
  bool released = false;
  loader.finish(std::make_unique<test_image>(behavior_a, &released));
  CHECK(released);
  CHECK_FALSE(scheduler.tasks[0]->active);
}
