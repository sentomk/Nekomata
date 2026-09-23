// PLT tests: activation rewrites the registered slots so ordinary call
// sites reach the new generation, without any application-side plumbing.
// Uses the session suite's mock loader/fetcher/scheduler idioms; the slots
// are real static-duration registrations, exactly what an application PLT
// header creates.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "session.hpp"
#include <neko/wasm/plt.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr std::string_view a_digest =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr std::string_view b_digest =
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

int g_behavior_a_calls = 0;
int g_behavior_b_calls = 0;

void behavior_a() {
  ++g_behavior_a_calls;
}
void behavior_b() {
  ++g_behavior_b_calls;
}

using slot = neko::wasm::plt_slot<void (*)()>;

// Static-duration slots, as an application PLT header declares them.
slot tick_slot{"game", "tick"};
slot identity_slot{"game", "identity"};
slot ghost_slot{"game", "ghost"};   // no module ever defines this entry
slot foreign_slot{"other", "tick"}; // another group's slot, never ours

class manual_scheduler final : public neko::wasm::poll_scheduler {
public:
  struct task {
    std::function<void()> callback;
    bool active = true;
  };
  class subscription final : public neko::wasm::poll_subscription {
  public:
    explicit subscription(std::shared_ptr<task> value) : task_(std::move(value)) {}
    ~subscription() override { task_->active = false; }

  private:
    std::shared_ptr<task> task_;
  };
  std::unique_ptr<neko::wasm::poll_subscription> repeat(std::function<void()> callback) override {
    auto value = std::make_shared<task>(task{std::move(callback)});
    tasks.push_back(value);
    return std::make_unique<subscription>(value);
  }
  void tick() {
    for (const auto& value : tasks) {
      if (value->active) {
        value->callback();
      }
    }
  }
  std::vector<std::shared_ptr<task>> tasks;
};

class test_image final : public neko::wasm::module_image {
public:
  explicit test_image(neko::wasm::module_function function) {
    entries[0] = {"tick", function};
    entries[1] = {"identity", function};
  }
  neko::wasm::module_entry entries[2];
  neko::wasm::module_descriptor value{
      {neko::wasm::module_interface_version, sizeof(neko::wasm::module_descriptor)},
      "test-v1",
      2,
      entries};
  const neko::wasm::module_header* descriptor() const noexcept override { return &value.header; }
  void keep_resident() noexcept override {}
};

class test_loader final : public neko::wasm::module_loader {
public:
  void open(std::string path, std::string_view,
            neko::wasm::module_loader::completion complete) override {
    static_cast<void>(path);
    pending = std::move(complete);
  }
  void finish(neko::wasm::module_function function) {
    auto callback = std::move(pending);
    callback({std::make_unique<test_image>(function), {}, neko::wasm::module_load_status::loaded});
  }
  neko::wasm::module_loader::completion pending;
};

class scripted_fetcher final : public neko::wasm::manifest_fetcher {
public:
  void fetch(std::string url, neko::wasm::manifest_fetcher::completion complete) override {
    static_cast<void>(url);
    pending = std::move(complete);
  }
  void deliver_ok(std::string text) {
    auto callback = std::move(pending);
    callback({true, std::move(text), {}});
  }
  neko::wasm::manifest_fetcher::completion pending;
};

std::string offer(std::uint64_t sequence, std::string_view generation, std::string_view digest) {
  return "nekomata-wasm/1\n"
         "group_id \"game\"\n"
         "sequence " +
         std::to_string(sequence) + "\n" + "generation_id \"" + std::string(generation) +
         "\"\n"
         "abi_id \"test-v1\"\n"
         "artifact \"modules/gen.wasm\" \"" +
         std::string(digest) + "\"\n" + "entry \"tick\"\n" + "entry \"identity\"\n";
}

// One preparation cycle: a scheduler tick polls, the manifest arrives, the
// artifact opens, and the loader hands back the image.
void prepare(manual_scheduler& scheduler, scripted_fetcher& fetcher, test_loader& loader,
             const std::string& text, neko::wasm::module_function body) {
  scheduler.tick();
  fetcher.deliver_ok(text);
  loader.finish(body);
}

} // namespace

TEST_CASE("activation rewrites this group's slots with the new entries") {
  test_loader loader;
  scripted_fetcher fetcher;
  manual_scheduler scheduler;
  neko::wasm::managed_session session{loader, fetcher, scheduler, {{"game", "offers/latest", {}}}};

  g_behavior_a_calls = 0;
  g_behavior_b_calls = 0;
  tick_slot.target = nullptr;
  identity_slot.target = nullptr;
  ghost_slot.target = nullptr;
  foreign_slot.target = nullptr;

  session.watch();
  prepare(scheduler, fetcher, loader, offer(1, "gen-1", a_digest), &behavior_a);

  const auto result = session.update();
  REQUIRE(result.events.size() == 1);
  CHECK(result.events[0].status == neko::update_status::applied);

  CHECK(tick_slot.target == &behavior_a);
  CHECK(identity_slot.target == &behavior_a);
  CHECK(ghost_slot.target == nullptr);   // entry absent from the module
  CHECK(foreign_slot.target == nullptr); // another group's slot

  tick_slot();
  identity_slot();
  CHECK(g_behavior_a_calls == 2);

  // A superseding generation rewrites the same slots; old code that caller
  // code still holds keeps answering through its old pointers.
  prepare(scheduler, fetcher, loader, offer(2, "gen-2", b_digest), &behavior_b);
  const auto second = session.update();
  REQUIRE(second.events.size() == 1);
  CHECK(second.events[0].status == neko::update_status::applied);
  CHECK(tick_slot.target == &behavior_b);
  tick_slot();
  CHECK(g_behavior_b_calls == 1);
  CHECK(g_behavior_a_calls == 2);
}

TEST_CASE("a rejected candidate leaves every slot untouched") {
  test_loader loader;
  scripted_fetcher fetcher;
  manual_scheduler scheduler;
  neko::wasm::managed_session session{loader, fetcher, scheduler, {{"game", "offers/latest", {}}}};

  tick_slot.target = &behavior_a;

  session.watch();
  scheduler.tick();
  fetcher.deliver_ok(offer(1, "gen-bad", a_digest));
  auto callback = std::move(loader.pending);
  callback({nullptr, "bad digest", neko::wasm::module_load_status::digest_mismatch});

  const auto result = session.update();
  REQUIRE(result.events.size() == 1);
  CHECK(result.events[0].status == neko::update_status::rejected);
  CHECK(tick_slot.target == &behavior_a); // the old generation still answers
}
