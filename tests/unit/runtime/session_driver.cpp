#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <neko/backend/session_driver.hpp>

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
class recording_driver final : public neko::backend::session_driver {
public:
  explicit recording_driver(bool& destroyed) : destroyed_(destroyed) {}
  ~recording_driver() override { destroyed_ = true; }
  void watch() override { calls.push_back("watch all"); }
  void watch(std::string_view id) override { calls.push_back("watch " + std::string{id}); }
  void unwatch() override { calls.push_back("unwatch all"); }
  void unwatch(std::string_view id) override { calls.push_back("unwatch " + std::string{id}); }
  neko::update_result update() override {
    calls.push_back("update");
    neko::update_event event;
    event.group_id = "test";
    event.generation_id = "generation";
    event.redirected_function_count = 2;
    return {{std::move(event)}};
  }
  neko::session_snapshot snapshot() const override {
    neko::session_snapshot out;
    out.applied = 7;
    out.last_result = "driver observation";
    return out;
  }
  std::vector<std::string> calls;

private:
  bool& destroyed_;
};
} // namespace

TEST_CASE("public session delegates lifecycle and owns the driver across moves") {
  bool destroyed = false;
  bool replaced_destroyed = false;
  {
    auto driver = std::make_unique<recording_driver>(destroyed);
    auto* recorded = driver.get();
    neko::reload_session original{std::move(driver)};
    original.watch();
    original.watch("test");
    original.watch(std::string_view{"other"});
    original.unwatch("test");
    original.unwatch(std::string_view{"other"});
    original.unwatch();
    neko::reload_session moved{std::move(original)};
    neko::reload_session target{std::make_unique<recording_driver>(replaced_destroyed)};
    target = std::move(moved);
    CHECK(replaced_destroyed);
    CHECK_FALSE(destroyed);
    const auto result = target.update();
    REQUIRE(result.events.size() == 1);
    CHECK(result.any_applied());
    CHECK(result.events[0].group_id == "test");
    CHECK(result.events[0].generation_id == "generation");
    CHECK(result.events[0].redirected_function_count == 2);
    const auto snapshot = std::as_const(target).snapshot();
    CHECK(snapshot.applied == 7);
    CHECK(snapshot.last_result == "driver observation");
    const std::vector<std::string> expected{"watch all",    "watch test",    "watch other",
                                            "unwatch test", "unwatch other", "unwatch all",
                                            "update"};
    CHECK(recorded->calls == expected);
    CHECK_THROWS_WITH_AS(target.watch(std::filesystem::path{"hot.o"}),
                         "reload_session: object watches are not supported by this backend",
                         std::runtime_error);
    CHECK_THROWS_WITH_AS(target.watch(std::filesystem::path{"hot.o"}, "hot.cpp"),
                         "reload_session: object watches are not supported by this backend",
                         std::runtime_error);
    CHECK(recorded->calls == expected);
  }
  CHECK(destroyed);
}

TEST_CASE("public session refuses a null lifecycle driver") {
  CHECK_THROWS_WITH_AS((neko::reload_session{std::unique_ptr<neko::backend::session_driver>{}}),
                       "reload_session: null session driver", std::runtime_error);
}
