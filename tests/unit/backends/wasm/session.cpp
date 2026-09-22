#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <backends/wasm/session.hpp>
#include <backends/wasm/session_error.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
using neko::group_state;
using neko::reload_error_code;
using neko::update_status;
using namespace neko::wasm;

static_assert(std::is_same_v<decltype(std::declval<managed_session&>().update()),
                             decltype(std::declval<neko::reload_session&>().update())>);
static_assert(std::is_same_v<decltype(std::declval<const managed_session&>().snapshot()),
                             neko::session_snapshot>);

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
    if (fail || tasks.size() == fail_after) {
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
  std::size_t fail_after = static_cast<std::size_t>(-1);
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

class routed_fetcher final : public manifest_fetcher {
public:
  void fetch(std::string url, completion complete) override {
    ++fetches[url];
    REQUIRE(pending.emplace(std::move(url), std::move(complete)).second);
  }
  void deliver(std::string_view group, std::string text) {
    auto request = pending.extract(std::string{group} + "/latest");
    REQUIRE_FALSE(request.empty());
    request.mapped()({true, std::move(text), {}});
  }
  std::map<std::string, completion> pending;
  std::map<std::string, unsigned> fetches;
};

class routed_loader final : public module_loader {
public:
  void open(std::string path, std::string_view sha256, completion complete) override {
    digests.push_back(std::string{sha256});
    REQUIRE(pending.emplace(std::move(path), std::move(complete)).second);
  }
  void finish(std::string_view group, std::unique_ptr<module_image> image) {
    auto request = pending.extract(std::string{group} + "/modules/gen.wasm");
    REQUIRE_FALSE(request.empty());
    request.mapped()(
        image ? module_load_result{std::move(image), {}, module_load_status::loaded}
              : module_load_result{nullptr, "bad digest", module_load_status::digest_mismatch});
  }
  std::map<std::string, completion> pending;
  std::vector<std::string> digests;
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

void check_group(const neko::session_snapshot& snapshot, bool enabled, group_state state,
                 std::uint64_t sequence, std::string_view last_applied = {}) {
  REQUIRE(snapshot.managed_groups.size() == 1);
  const auto& group = snapshot.managed_groups.front();
  CHECK(group.group_id == "game");
  CHECK(group.enabled == enabled);
  CHECK(group.state == state);
  CHECK(group.observed_sequence == sequence);
  CHECK(group.last_applied_generation == last_applied);
  CHECK(snapshot.watched_paths.empty());
}

} // namespace

TEST_CASE("group registration rejects empty and duplicate identities before observation") {
  routed_loader loader;
  routed_fetcher fetcher;
  manual_scheduler scheduler;
  REQUIRE_THROWS_WITH_AS(
      (managed_session{loader, fetcher, scheduler, {{"b", "b/latest", {}}, {"", "a/latest", {}}}}),
      "reload_session: expected group must not be empty", std::runtime_error);
  REQUIRE_THROWS_WITH_AS(
      (managed_session{loader,
                       fetcher,
                       scheduler,
                       {{"b", "b/latest", {}}, {"a", "a/latest", {}}, {"b", "other/latest", {}}}}),
      "reload_session: duplicate reload group 'b'", std::runtime_error);
  CHECK(scheduler.tasks.empty());
  CHECK(fetcher.fetches.empty());
  CHECK(loader.digests.empty());

  managed_session empty{loader, fetcher, scheduler, {}};
  CHECK(empty.snapshot().managed_groups.empty());
  CHECK(empty.update().events.empty());
  CHECK_NOTHROW(empty.unwatch());
  REQUIRE_THROWS_WITH_AS(empty.watch(),
                         "reload_session: no registered reload groups; nothing to watch",
                         std::runtime_error);
  REQUIRE_THROWS_WITH_AS(static_cast<void>(empty.current("missing")),
                         "reload_session: unknown reload group 'missing'", std::runtime_error);
}

TEST_CASE("group selection is independent and registration and result order are deterministic") {
  routed_loader loader;
  routed_fetcher fetcher;
  manual_scheduler scheduler;
  managed_session session{
      loader, fetcher, scheduler, {{"b", "b/latest", {}}, {"a", "a/latest", {}}}};
  const auto initial = session.snapshot();
  REQUIRE(initial.managed_groups.size() == 2);
  CHECK(initial.managed_groups[0].group_id == "a");
  CHECK(initial.managed_groups[1].group_id == "b");
  for (const auto& group : initial.managed_groups) {
    CHECK_FALSE(group.enabled);
    CHECK(group.state == group_state::idle);
    CHECK(group.observed_sequence == 0);
    CHECK(group.last_applied_generation.empty());
    CHECK(session.current(group.group_id) == nullptr);
  }
  CHECK(initial.applied == 0);
  CHECK(initial.rejected == 0);
  CHECK(initial.last_result.empty());
  CHECK(initial.watched_paths.empty());
  session.watch("b");
  session.watch("b");
  REQUIRE(scheduler.tasks.size() == 1);
  REQUIRE_THROWS_WITH_AS(session.watch("missing"), "reload_session: unknown reload group 'missing'",
                         std::runtime_error);
  REQUIRE_THROWS_WITH_AS(session.unwatch("missing"),
                         "reload_session: unknown reload group 'missing'", std::runtime_error);
  REQUIRE_THROWS_WITH_AS(static_cast<void>(std::as_const(session).current("missing")),
                         "reload_session: unknown reload group 'missing'", std::runtime_error);
  const auto selected = session.snapshot();
  CHECK_FALSE(selected.managed_groups[0].enabled);
  CHECK(selected.managed_groups[1].enabled);
  session.watch();
  session.watch();
  REQUIRE(scheduler.tasks.size() == 2);
  scheduler.tick();
  CHECK(fetcher.fetches.at("a/latest") == 1);
  CHECK(fetcher.fetches.at("b/latest") == 1);
  // Identical sequence and generation IDs belong to separate group streams.
  fetcher.deliver("b", offer_text(7, "same-generation", b_digest, "b"));
  fetcher.deliver("a", offer_text(7, "same-generation", a_digest, "a"));
  loader.finish("b", std::make_unique<test_image>(behavior_b));
  loader.finish("a", std::make_unique<test_image>(behavior_a));
  const auto ready = session.snapshot();
  CHECK(ready.applied == 0);
  for (const auto& group : ready.managed_groups) {
    CHECK(group.state == group_state::ready);
    CHECK(group.observed_sequence == 7);
    CHECK(session.current(group.group_id) == nullptr);
  }
  const auto result = session.update();
  REQUIRE(result.events.size() == 2);
  CHECK(result.events[0].group_id == "a");
  CHECK(result.events[1].group_id == "b");
  for (const auto& event : result.events) {
    CHECK(event.status == update_status::applied);
    CHECK(event.code == reload_error_code::none);
    CHECK(event.generation_id == "same-generation");
    CHECK(event.redirected_function_count == 2);
    CHECK(event.message.empty());
  }
  REQUIRE(session.current("a") != nullptr);
  REQUIRE(session.current("b") != nullptr);
  CHECK(session.current("a")->entry("tick") == behavior_a);
  CHECK(session.current("b")->entry("tick") == behavior_b);
  CHECK(session.snapshot().applied == 2);
  CHECK(session.snapshot().rejected == 0);
  CHECK(session.snapshot().last_result == "applied generation 'same-generation': 2 function(s)");
  CHECK(session.update().events.empty());
  CHECK(ready.applied == 0);
  CHECK(ready.managed_groups[0].state == group_state::ready);
  CHECK_FALSE(initial.managed_groups[1].enabled);
  session.unwatch();
  session.unwatch();
  scheduler.tick();
  for (const auto& task : scheduler.tasks) {
    CHECK_FALSE(task->active);
  }
  CHECK(fetcher.fetches.at("a/latest") == 1);
  CHECK(fetcher.fetches.at("b/latest") == 1);
}

TEST_CASE("a paused group retains late work while another continues to apply") {
  routed_loader loader;
  routed_fetcher fetcher;
  manual_scheduler scheduler;
  managed_session session{
      loader, fetcher, scheduler, {{"b", "b/latest", {}}, {"a", "a/latest", {}}}};
  session.watch();
  scheduler.tick();
  const auto old_a_tick = scheduler.tasks[0]->callback;
  session.unwatch("a");
  session.unwatch("a");
  fetcher.deliver("a", offer_text(10, "a-10", a_digest, "a"));
  fetcher.deliver("b", offer_text(1, "b-1", b_digest, "b"));
  bool reject_a = false;
  SUBCASE("paused ready candidate") {}
  SUBCASE("paused rejection") {
    reject_a = true;
  }
  loader.finish("a", reject_a ? nullptr : std::make_unique<test_image>(behavior_a));
  loader.finish("b", std::make_unique<test_image>(behavior_b));
  const auto paused = session.snapshot();
  REQUIRE(paused.managed_groups.size() == 2);
  CHECK_FALSE(paused.managed_groups[0].enabled);
  CHECK(paused.managed_groups[0].state == (reject_a ? group_state::failed : group_state::ready));
  CHECK(paused.managed_groups[0].observed_sequence == 10);
  CHECK(paused.managed_groups[1].state == group_state::ready);
  auto first = session.update();
  REQUIRE(first.events.size() == 1);
  CHECK(first.events[0].group_id == "b");
  CHECK(first.any_applied());
  CHECK(session.current("a") == nullptr);
  const auto b_first = session.current("b");
  scheduler.tick();
  old_a_tick();
  CHECK(fetcher.fetches.at("a/latest") == 1);
  fetcher.deliver("b", offer_text(2, "b-2", a_digest, "b"));
  loader.finish("b", std::make_unique<test_image>(behavior_a));
  auto second = session.update();
  REQUIRE(second.events.size() == 1);
  CHECK(second.events[0].group_id == "b");
  CHECK(second.events[0].generation_id == "b-2");
  REQUIRE(b_first != nullptr);
  CHECK(b_first->entry("tick") == behavior_b);
  CHECK(session.current("b")->entry("tick") == behavior_a);
  session.watch("a");
  old_a_tick();
  CHECK(fetcher.fetches.at("a/latest") == 1);
  auto resumed = session.update();
  REQUIRE(resumed.events.size() == 1);
  CHECK(resumed.events[0].group_id == "a");
  CHECK(resumed.events[0].generation_id == "a-10");
  CHECK(resumed.events[0].status == (reject_a ? update_status::rejected : update_status::applied));
  CHECK(resumed.events[0].code ==
        (reject_a ? reload_error_code::integrity : reload_error_code::none));
  CHECK(resumed.events[0].redirected_function_count == (reject_a ? 0 : 2));
  CHECK(resumed.any_applied() == !reject_a);
  CHECK(session.update().events.empty());
  const auto after = session.snapshot();
  CHECK(after.applied == (reject_a ? 2 : 3));
  CHECK(after.rejected == (reject_a ? 1 : 0));
  CHECK(after.last_result ==
        (reject_a ? "bad digest" : "applied generation 'a-10': 2 function(s)"));
  CHECK(after.managed_groups[0].last_applied_generation == (reject_a ? "" : "a-10"));
  CHECK(after.managed_groups[1].last_applied_generation == "b-2");
  scheduler.tick();
  fetcher.deliver("a", offer_text(10, "a-10", a_digest, "a"));
  fetcher.deliver("b", offer_text(2, "b-2", a_digest, "b"));
  CHECK(loader.digests.size() == 3);
  CHECK(session.update().events.empty());
}

TEST_CASE("a group rejection never blocks another group's transaction") {
  routed_loader loader;
  routed_fetcher fetcher;
  manual_scheduler scheduler;
  managed_session session{
      loader, fetcher, scheduler, {{"b", "b/latest", {}}, {"a", "a/latest", {}}}};
  session.watch();
  scheduler.tick();
  for (const auto* id : {"a", "b"}) {
    fetcher.deliver(id, offer_text(1, "baseline", a_digest, id));
    loader.finish(id, std::make_unique<test_image>(behavior_a));
  }
  REQUIRE(session.update().events.size() == 2);
  const auto old_a = session.current("a");
  const auto old_b = session.current("b");
  bool reject_a = true;
  SUBCASE("earlier group rejected") {}
  SUBCASE("later group rejected") {
    reject_a = false;
  }
  scheduler.tick();
  fetcher.deliver("b", offer_text(2, "b-2", b_digest, "b"));
  fetcher.deliver("a", offer_text(2, "a-2", b_digest, "a"));
  loader.finish("b", reject_a ? std::make_unique<test_image>(behavior_b) : nullptr);
  loader.finish("a", reject_a ? nullptr : std::make_unique<test_image>(behavior_b));
  const auto result = session.update();
  REQUIRE(result.events.size() == 2);
  CHECK(result.events[0].group_id == "a");
  CHECK(result.events[1].group_id == "b");
  CHECK(result.any_applied());
  const auto& failed = result.events[reject_a ? 0 : 1];
  const auto& applied = result.events[reject_a ? 1 : 0];
  CHECK(failed.status == update_status::rejected);
  CHECK(failed.code == reload_error_code::integrity);
  CHECK(failed.message == "bad digest");
  CHECK(failed.redirected_function_count == 0);
  CHECK(applied.status == update_status::applied);
  CHECK(applied.redirected_function_count == 2);
  CHECK(session.current(reject_a ? "a" : "b") == (reject_a ? old_a : old_b));
  CHECK(session.current(reject_a ? "b" : "a")->entry("tick") == behavior_b);
  const auto snapshot = session.snapshot();
  CHECK(snapshot.applied == 3);
  CHECK(snapshot.rejected == 1);
  CHECK(snapshot.last_result ==
        (reject_a ? "applied generation 'b-2': 2 function(s)" : "bad digest"));
  CHECK(snapshot.managed_groups[0].last_applied_generation == (reject_a ? "baseline" : "a-2"));
  CHECK(snapshot.managed_groups[1].last_applied_generation == (reject_a ? "b-2" : "baseline"));
  CHECK(session.update().events.empty());
}

TEST_CASE("per-group diagnostics and incomplete work cannot contaminate another stream") {
  routed_loader loader;
  routed_fetcher fetcher;
  manual_scheduler scheduler;
  diagnostics_log a_log;
  diagnostics_log b_log;
  managed_session session{
      loader,
      fetcher,
      scheduler,
      {{"a", "a/latest", [&a_log](const offer_event& event) { a_log.record(event); }},
       {"b", "b/latest", [&b_log](const offer_event& event) { b_log.record(event); }}}};
  session.watch();
  scheduler.tick();
  // A manifest delivered to the wrong subscription never routes into that group.
  fetcher.deliver("b", offer_text(100, "wrong-stream", b_digest, "a"));
  REQUIRE(b_log.kinds.size() == 1);
  CHECK(b_log.kinds[0] == offer_event_kind::offer_ignored);
  CHECK(b_log.messages[0] == "offer group 'a' does not match watched group 'b'");
  CHECK(a_log.kinds.empty());
  CHECK(loader.pending.empty());
  CHECK(session.update().events.empty());
  const auto ignored = session.snapshot();
  REQUIRE(ignored.managed_groups.size() == 2);
  CHECK(ignored.managed_groups[0].observed_sequence == 0);
  CHECK(ignored.managed_groups[1].observed_sequence == 0);
  CHECK(ignored.applied == 0);
  CHECK(ignored.rejected == 0);
  CHECK(ignored.last_result.empty());
  fetcher.deliver("a", offer_text(1, "a-1", a_digest, "a"));
  loader.finish("a", std::make_unique<test_image>(behavior_a));
  scheduler.tick();
  fetcher.deliver("b", offer_text(1, "b-1", b_digest, "b"));
  // B is still loading; it does not delay A's safe-point commit.
  const auto result = session.update();
  REQUIRE(result.events.size() == 1);
  CHECK(result.events[0].group_id == "a");
  CHECK(result.any_applied());
  CHECK(session.current("b") == nullptr);
  REQUIRE(a_log.kinds.size() == 1);
  CHECK(a_log.kinds[0] == offer_event_kind::offer_accepted);
  REQUIRE(b_log.kinds.size() == 2);
  CHECK(b_log.kinds[1] == offer_event_kind::offer_accepted);
  const auto active_a = session.current("a");
  loader.finish("b", std::make_unique<test_image>(behavior_b));
  const auto second = session.update();
  REQUIRE(second.events.size() == 1);
  CHECK(second.events[0].group_id == "b");
  CHECK(second.any_applied());
  CHECK(session.current("a") == active_a);
  CHECK(session.snapshot().applied == 2);
  CHECK(session.snapshot().rejected == 0);
}

TEST_CASE("all-group scheduling failure preserves enabled groups and remains retryable") {
  routed_loader loader;
  routed_fetcher fetcher;
  manual_scheduler scheduler;
  managed_session session{
      loader, fetcher, scheduler, {{"b", "b/latest", {}}, {"a", "a/latest", {}}}};
  scheduler.fail_after = 1;
  REQUIRE_THROWS_WITH_AS(session.watch(), "fixture scheduler failed", std::runtime_error);
  const auto partial = session.snapshot();
  REQUIRE(partial.managed_groups.size() == 2);
  CHECK(partial.managed_groups[0].enabled);
  CHECK_FALSE(partial.managed_groups[1].enabled);
  scheduler.tick();
  CHECK(fetcher.fetches.size() == 1);
  CHECK(fetcher.fetches.at("a/latest") == 1);
  scheduler.fail_after = static_cast<std::size_t>(-1);
  session.watch();
  REQUIRE(scheduler.tasks.size() == 2);
  scheduler.tick();
  CHECK(fetcher.fetches.at("a/latest") == 1);
  CHECK(fetcher.fetches.at("b/latest") == 1);
  CHECK(session.snapshot().managed_groups[1].enabled);
}

TEST_CASE("destroying multiple groups invalidates all pending callbacks") {
  routed_loader loader;
  routed_fetcher fetcher;
  manual_scheduler scheduler;
  {
    managed_session session{
        loader, fetcher, scheduler, {{"b", "b/latest", {}}, {"a", "a/latest", {}}}};
    session.watch();
    scheduler.tick();
    fetcher.deliver("a", offer_text(1, "a-1", a_digest, "a"));
  }
  REQUIRE(scheduler.tasks.size() == 2);
  for (const auto& task : scheduler.tasks) {
    CHECK_FALSE(task->active);
    task->callback();
  }
  bool released = false;
  loader.finish("a", std::make_unique<test_image>(behavior_a, &released));
  CHECK(released);
  fetcher.deliver("b", offer_text(1, "b-1", b_digest, "b"));
  CHECK(loader.digests.size() == 1);
  CHECK(fetcher.fetches.at("a/latest") == 1);
  CHECK(fetcher.fetches.at("b/latest") == 1);
}

TEST_CASE("snapshots are independent values and never start observation") {
  test_loader loader;
  scripted_fetcher fetcher;
  manual_scheduler scheduler;
  managed_session session{loader, fetcher, scheduler, {{"game", "offers/latest", {}}}};
  auto initial = session.snapshot();
  check_group(initial, false, group_state::idle, 0);
  CHECK(initial.applied == 0);
  CHECK(initial.rejected == 0);
  CHECK(initial.last_result.empty());
  session.watch();
  const auto enabled = session.snapshot();
  check_group(enabled, true, group_state::preparing, 0);
  check_group(initial, false, group_state::idle, 0);
  initial.managed_groups.front().group_id = "changed copy";
  initial.managed_groups.front().last_applied_generation = "not applied";
  initial.applied = 100;
  initial.last_result = "not a transaction";
  check_group(session.snapshot(), true, group_state::preparing, 0);
  CHECK(session.snapshot().applied == 0);
  CHECK(session.snapshot().last_result.empty());
  session.unwatch();
  check_group(session.snapshot(), false, group_state::idle, 0);
  CHECK(session.update().events.empty());
  CHECK(fetcher.fetches == 0);
  CHECK(loader.opened_paths.empty());
}

TEST_CASE("snapshots distinguish paused preparation from consumed transactions") {
  test_loader loader;
  scripted_fetcher fetcher;
  manual_scheduler scheduler;
  managed_session session{loader, fetcher, scheduler, {{"game", "offers/latest", {}}}};
  session.watch();
  poll(scheduler, fetcher, offer_text(7, "gen-7", a_digest));
  check_group(session.snapshot(), true, group_state::preparing, 7);
  loader.finish(std::make_unique<test_image>(behavior_a));
  const auto ready_a = session.snapshot();
  check_group(ready_a, true, group_state::ready, 7);
  CHECK(ready_a.applied == 0);
  CHECK(ready_a.last_result.empty());
  REQUIRE(session.update().any_applied());
  const auto active = session.current("game");
  const auto applied_a = session.snapshot();
  check_group(applied_a, true, group_state::preparing, 7, "gen-7");
  CHECK(applied_a.applied == 1);
  CHECK(applied_a.last_result == "applied generation 'gen-7': 2 function(s)");

  scheduler.tick();
  session.unwatch();
  // The cursor advances on a late accepted manifest, not on activation.
  fetcher.deliver_ok(offer_text(8, "gen-8", b_digest));
  const auto loading_b = session.snapshot();
  check_group(loading_b, false, group_state::idle, 8, "gen-7");
  bool reject = false;
  SUBCASE("ready while disabled") {
    loader.finish(std::make_unique<test_image>(behavior_b));
  }
  SUBCASE("failed while disabled") {
    reject = true;
    loader.finish(nullptr, "bad digest", module_load_status::digest_mismatch);
  }
  const auto pending_state = reject ? group_state::failed : group_state::ready;
  const auto pending_b = session.snapshot();
  check_group(pending_b, false, pending_state, 8, "gen-7");
  CHECK(pending_b.applied == 1);
  CHECK(pending_b.rejected == 0);
  CHECK(pending_b.last_result == applied_a.last_result);
  CHECK(session.update().events.empty());
  CHECK(session.current("game") == active);
  CHECK(session.snapshot().rejected == 0);
  CHECK(fetcher.fetches == 2);
  CHECK(loader.opened_paths.size() == 2);

  session.watch("game");
  check_group(session.snapshot(), true, pending_state, 8, "gen-7");
  REQUIRE(session.update().events.size() == 1);
  const auto consumed = session.snapshot();
  check_group(consumed, true, reject ? group_state::failed : group_state::preparing, 8,
              reject ? "gen-7" : "gen-8");
  CHECK(consumed.applied == (reject ? 1 : 2));
  CHECK(consumed.rejected == (reject ? 1 : 0));
  CHECK(consumed.last_result ==
        (reject ? "bad digest" : "applied generation 'gen-8': 2 function(s)"));
  CHECK(session.update().events.empty());
  session.unwatch();
  check_group(session.snapshot(), false, reject ? group_state::failed : group_state::idle, 8,
              reject ? "gen-7" : "gen-8");
  CHECK(session.snapshot().applied == consumed.applied);
  CHECK(session.snapshot().rejected == consumed.rejected);
  CHECK(session.snapshot().last_result == consumed.last_result);
  // Old snapshots do not change as the live session advances.
  check_group(ready_a, true, group_state::ready, 7);
  CHECK(ready_a.applied == 0);
  check_group(loading_b, false, group_state::idle, 8, "gen-7");
  check_group(pending_b, false, pending_state, 8, "gen-7");
}

TEST_CASE("superseded pending results do not enter snapshot transaction counts") {
  test_loader loader;
  scripted_fetcher fetcher;
  manual_scheduler scheduler;
  managed_session session{loader, fetcher, scheduler, {{"game", "offers/latest", {}}}};
  session.watch();
  poll(scheduler, fetcher, offer_text(7, "gen-7", a_digest));
  SUBCASE("superseded ready generation") {
    loader.finish(std::make_unique<test_image>(behavior_a));
    check_group(session.snapshot(), true, group_state::ready, 7);
  }
  SUBCASE("superseded rejection") {
    loader.finish(nullptr, "bad digest", module_load_status::digest_mismatch);
    check_group(session.snapshot(), true, group_state::failed, 7);
  }
  poll(scheduler, fetcher, offer_text(8, "gen-8", b_digest));
  check_group(session.snapshot(), true, group_state::preparing, 8);
  CHECK(session.snapshot().applied == 0);
  CHECK(session.snapshot().rejected == 0);
  CHECK(session.snapshot().last_result.empty());
  loader.finish(std::make_unique<test_image>(behavior_b));
  check_group(session.snapshot(), true, group_state::ready, 8);
  REQUIRE(session.update().any_applied());
  check_group(session.snapshot(), true, group_state::preparing, 8, "gen-8");
  CHECK(session.snapshot().applied == 1);
  CHECK(session.snapshot().rejected == 0);
}

TEST_CASE("observation diagnostics preserve snapshot cursor and transaction history") {
  test_loader loader;
  scripted_fetcher fetcher;
  manual_scheduler scheduler;
  diagnostics_log log;
  managed_session session{
      loader, fetcher, scheduler, {{"game", "offers/latest", [&log](const offer_event& event) {
                                      log.record(event);
                                    }}}};
  session.watch();
  poll(scheduler, fetcher, offer_text(8, "gen-8", b_digest));
  loader.finish(nullptr, "bad digest", module_load_status::digest_mismatch);
  REQUIRE(session.update().events.size() == 1);
  const auto before = session.snapshot();
  poll(scheduler, fetcher, offer_text(8, "gen-8", b_digest));
  poll(scheduler, fetcher, offer_text(7, "gen-7", a_digest));
  poll(scheduler, fetcher, offer_text(8, "conflict", b_digest));
  poll(scheduler, fetcher, offer_text(100, "other", b_digest, "other-game"));
  poll(scheduler, fetcher, "not a manifest");
  scheduler.tick();
  auto complete = std::move(fetcher.pending);
  complete({false, {}, "transport unavailable"});
  CHECK(log.kinds.size() == 7);
  CHECK(session.update().events.empty());
  check_group(session.snapshot(), true, group_state::failed, 8);
  CHECK(session.snapshot().applied == 0);
  CHECK(session.snapshot().rejected == 1);
  CHECK(session.snapshot().last_result == before.last_result);
  CHECK(loader.opened_paths.size() == 1);
  poll(scheduler, fetcher, offer_text(9, "gen-9", a_digest));
  check_group(session.snapshot(), true, group_state::preparing, 9);
  CHECK(session.snapshot().last_result == before.last_result);
  loader.finish(std::make_unique<test_image>(behavior_a));
  check_group(session.snapshot(), true, group_state::ready, 9);
  CHECK(session.snapshot().applied == 0);
  CHECK(session.snapshot().rejected == 1);
  CHECK(session.snapshot().last_result == before.last_result);
  REQUIRE(session.update().any_applied());
  check_group(session.snapshot(), true, group_state::preparing, 9, "gen-9");
  CHECK(session.snapshot().applied == 1);
  CHECK(session.snapshot().rejected == 1);
  CHECK(session.snapshot().last_result == "applied generation 'gen-9': 2 function(s)");
}

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
  managed_session session{loader, fetcher, scheduler, {{"physics", "offers/latest", {}}}};
  session.watch();
  poll(scheduler, fetcher, offer_text(7, "gen-7", a_digest, "physics"));
  loader.finish(std::make_unique<test_image>(behavior_a));
  REQUIRE(session.update().any_applied());
  const auto active = session.current("physics");

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
  CHECK(session.current("physics") == active);
  CHECK(session.current("physics")->entry("tick") == behavior_a);
  CHECK(session.update().events.empty());
}

TEST_CASE("an empty expected group is a configuration error") {
  test_loader loader;
  scripted_fetcher fetcher;
  manual_scheduler scheduler;
  REQUIRE_THROWS_AS((managed_session{loader, fetcher, scheduler, {{"", "offers/latest", {}}}}),
                    std::runtime_error);
}

TEST_CASE("a ready generation applies at the update safe point") {
  test_loader loader;
  scripted_fetcher fetcher;
  manual_scheduler scheduler;
  managed_session session{loader, fetcher, scheduler, {{"game", "offers/latest", {}}}};
  session.watch();
  REQUIRE(session.current("game") == nullptr);
  CHECK(session.update().events.empty());

  poll(scheduler, fetcher, offer_text(7, "gen-7", a_digest));

  // The offer was accepted with the manifest fetch; the candidate is still
  // loading, so nothing applies yet.
  auto loading = session.update();
  CHECK(loading.events.empty());
  CHECK_FALSE(loading.any_applied());
  CHECK(session.current("game") == nullptr);

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
  REQUIRE(session.current("game") != nullptr);
  CHECK(session.current("game")->entry_count() == 2);
  CHECK(session.current("game")->entry("tick") == behavior_a);

  // Reported exactly once: later updates stay quiet.
  CHECK(session.update().events.empty());
}

TEST_CASE("a rejected generation reports once and the active set survives") {
  test_loader loader;
  scripted_fetcher fetcher;
  manual_scheduler scheduler;
  managed_session session{loader, fetcher, scheduler, {{"game", "offers/latest", {}}}};
  session.watch();

  poll(scheduler, fetcher, offer_text(7, "gen-7", a_digest));
  loader.finish(std::make_unique<test_image>(behavior_a));
  REQUIRE(session.update().events.front().status == update_status::applied);
  const auto active = session.current("game");
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

  CHECK(session.current("game") == active);
  CHECK(session.current("game")->entry("tick") == behavior_a);
  CHECK(session.update().events.empty());
}

TEST_CASE("a superseding generation activates after an applied one") {
  test_loader loader;
  scripted_fetcher fetcher;
  manual_scheduler scheduler;
  managed_session session{loader, fetcher, scheduler, {{"game", "offers/latest", {}}}};
  session.watch();

  poll(scheduler, fetcher, offer_text(7, "gen-7", a_digest));
  loader.finish(std::make_unique<test_image>(behavior_a));
  REQUIRE(session.update().events.front().status == update_status::applied);
  const auto first = session.current("game");

  poll(scheduler, fetcher, offer_text(8, "gen-8", b_digest));
  loader.finish(std::make_unique<test_image>(behavior_b));
  auto second = session.update();
  REQUIRE(second.events.front().status == update_status::applied);
  CHECK(second.events.front().generation_id == "gen-8");
  CHECK(session.current("game") != first);
  CHECK(session.current("game")->entry("tick") == behavior_b);
  // The previous snapshot keeps resolving to its own generation's code.
  CHECK(first->entry("tick") == behavior_a);
}

TEST_CASE("offers for another group are diagnostics, not transactions") {
  test_loader loader;
  scripted_fetcher fetcher;
  manual_scheduler scheduler;
  diagnostics_log log;
  managed_session session{
      loader, fetcher, scheduler, {{"game", "offers/latest", [&log](const offer_event& event) {
                                      log.record(event);
                                    }}}};
  session.watch();

  poll(scheduler, fetcher, offer_text(7, "gen-7", a_digest, "other-game"));
  CHECK(session.update().events.empty());
  CHECK(session.current("game") == nullptr);
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
  managed_session session{loader, fetcher, scheduler, {{"game", "offers/latest", {}}}};
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
  managed_session session{loader, fetcher, scheduler, {{"game", "offers/latest", {}}}};
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
  CHECK(session.current("game") == nullptr);
  REQUIRE(session.update().events.size() == 1);
  CHECK(session.current("game")->entry("tick") == behavior_a);
  CHECK(fetcher.fetches == 1); // Commit never starts another fetch.
}

TEST_CASE("unknown watch and unwatch groups leave the subscription unchanged") {
  test_loader loader;
  scripted_fetcher fetcher;
  manual_scheduler scheduler;
  managed_session session{loader, fetcher, scheduler, {{"game", "offers/latest", {}}}};
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
  managed_session session{loader, fetcher, scheduler, {{"game", "offers/latest", {}}}};
  session.watch();
  poll(scheduler, fetcher, offer_text(7, "gen-7", a_digest));
  loader.finish(std::make_unique<test_image>(behavior_a));
  REQUIRE(session.update().events.size() == 1);
  const auto active = session.current("game");
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
  CHECK(session.current("game") == active);
  CHECK(fetcher.fetches == 2);
  session.watch("game");
  CHECK(session.current("game") == active);
  const auto applied = session.update();
  REQUIRE(applied.events.size() == 1);
  CHECK(applied.events[0].generation_id == "gen-8");
  CHECK(applied.events[0].status == update_status::applied);
  CHECK(session.current("game")->entry("tick") == behavior_b);
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
  managed_session session{loader, fetcher, scheduler, {{"game", "offers/latest", {}}}};
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
  CHECK(session.current("game") == nullptr);
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
  managed_session session{loader, fetcher, scheduler, {{"game", "offers/latest", {}}}};
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
    managed_session session{loader, fetcher, scheduler, {{"game", "offers/latest", {}}}};
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
  managed_session session{loader, fetcher, scheduler, {{"game", "offers/latest", {}}}};
  scheduler.fail = true;
  REQUIRE_THROWS_WITH_AS(session.watch(), "fixture scheduler failed", std::runtime_error);
  check_group(session.snapshot(), false, group_state::idle, 0);
  CHECK(session.update().events.empty());
  CHECK(fetcher.fetches == 0);
  scheduler.fail = false;
  session.watch();
  check_group(session.snapshot(), true, group_state::preparing, 0);
  scheduler.tick();
  CHECK(fetcher.fetches == 1);
}

TEST_CASE("an already rejected candidate remains reportable after pause") {
  test_loader loader;
  scripted_fetcher fetcher;
  manual_scheduler scheduler;
  managed_session session{loader, fetcher, scheduler, {{"game", "offers/latest", {}}}};
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

TEST_CASE("entry snapshots pin generations beyond session lifetime") {
  test_loader loader;
  scripted_fetcher fetcher;
  manual_scheduler scheduler;
  std::shared_ptr<const prepared_module> first;
  std::shared_ptr<const prepared_module> last;
  bool released = false;
  {
    managed_session session{loader,
                            fetcher,
                            scheduler,
                            {{"game", "offers/latest", {}, {"test-v1", {"tick", "identity"}, {}}}}};
    session.watch();
    poll(scheduler, fetcher, offer_text(1, "a", a_digest));
    loader.finish(std::make_unique<test_image>(behavior_a, &released));
    CHECK_FALSE(session.current("game"));
    REQUIRE(session.update().any_applied());
    first = session.current("game");
    CHECK(first->entry("tick") == behavior_a);
    CHECK(first->entry("identity") == behavior_a);
    poll(scheduler, fetcher, offer_text(2, "b", b_digest));
    loader.finish(std::make_unique<test_image>(behavior_b));
    REQUIRE(session.update().any_applied());
    CHECK(session.current("game")->entry("tick") == behavior_b);
    CHECK(first->entry("identity") == behavior_a);
    last = session.current("game");
    session.unwatch();
  }
  CHECK_FALSE(released);
  first->entry("tick")();
  last->entry("tick")();
  first = {};
  CHECK(released);
}

TEST_CASE("registered host contract pins ABI and exact entry membership before loading") {
  test_loader loader;
  scripted_fetcher fetcher;
  manual_scheduler scheduler;
  managed_session session{loader,
                          fetcher,
                          scheduler,
                          {{"game", "offers/latest", {}, {"test-v1", {"tick", "identity"}, {}}}}};
  session.watch();
  poll(scheduler, fetcher, offer_text(1, "a", a_digest));
  loader.finish(std::make_unique<test_image>(behavior_a));
  REQUIRE(session.update().any_applied());
  auto incompatible = offer_text(2, "b", b_digest);
  SUBCASE("ABI changed by publisher") {
    incompatible.replace(incompatible.find("test-v1"), 7, "test-v2");
  }
  SUBCASE("entries reordered") {
    incompatible.replace(incompatible.find("entry \"tick\""), std::string::npos,
                         "entry \"identity\"\nentry \"tick\"\n");
  }
  SUBCASE("entry removed") {
    incompatible.erase(incompatible.find("entry \"identity\""));
  }
  SUBCASE("entry added") {
    incompatible += "entry \"extra\"\n";
  }
  poll(scheduler, fetcher, incompatible);
  REQUIRE(loader.opened_paths.size() == 1);
  CHECK(session.snapshot().managed_groups[0].observed_sequence == 2);
  session.unwatch();
  CHECK(session.update().events.empty());
  session.watch();
  const auto result = session.update();
  REQUIRE(result.events.size() == 1);
  CHECK(result.events[0].code == reload_error_code::incompatible);
  CHECK(result.events[0].message ==
        "offer ABI or entry membership does not match the registered host contract");
  CHECK(session.current("game")->entry("tick") == behavior_a);
  CHECK(session.update().events.empty());
  poll(scheduler, fetcher, incompatible);
  CHECK(session.update().events.empty());
  CHECK(loader.opened_paths.size() == 1);
}

TEST_CASE("destruction discards an outstanding artifact completion") {
  test_loader loader;
  scripted_fetcher fetcher;
  manual_scheduler scheduler;
  {
    managed_session session{loader, fetcher, scheduler, {{"game", "offers/latest", {}}}};
    session.watch();
    poll(scheduler, fetcher, offer_text(7, "gen-7", a_digest));
  }
  bool released = false;
  loader.finish(std::make_unique<test_image>(behavior_a, &released));
  CHECK(released);
  CHECK_FALSE(scheduler.tasks[0]->active);
}
