// Managed watch/unwatch semantics of reload_session for programs without
// embedded descriptors. The end-to-end managed path runs on Linux with a
// linked descriptor section (neko.managed_reload).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <neko/backend.hpp>
#include <neko/backend/code_substituter.hpp>
#include <neko/backend/object_loader.hpp>
#include <neko/backend/state_manager.hpp>
#include <neko/backend/symbol_provider.hpp>
#include <neko/session.hpp>

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string_view>

namespace {

class fake_process final : public neko::backend::symbol_provider,
                           public neko::backend::state_manager {
public:
  std::vector<neko::backend::function_info> all_functions() const override { return {}; }

  std::optional<neko::backend::function_info> function_by_name(std::string_view) const override {
    return std::nullopt;
  }

  std::size_t count_functions(std::string_view) const override { return 0; }

  std::optional<neko::backend::global_variable> global_by_name(std::string_view) const override {
    return std::nullopt;
  }

  std::size_t count_globals(std::string_view) const override { return 0; }

  neko::backend::type_layout layout_of(neko::backend::type_id id) const override {
    neko::backend::type_layout layout;
    layout.id = id;
    return layout;
  }

  void* map_global(std::string_view) override { return nullptr; }
};

class stub_loader final : public neko::backend::object_loader {
public:
  neko::backend::loaded_image load(const std::uint8_t*, std::size_t) override {
    throw std::runtime_error("unexpected object load");
  }
};

class stub_substituter final : public neko::backend::code_substituter {
public:
  neko::backend::executable_allocation_ptr reserve_code_near(std::uintptr_t,
                                                             std::uint64_t) override {
    return nullptr;
  }

  bool commit_code(neko::backend::executable_allocation&, const void*, std::uint64_t) override {
    return false;
  }

  bool precheck_entry(std::uintptr_t, void*) override { return false; }

  bool snapshot_entry(std::uintptr_t, std::uint8_t[5]) override { return false; }

  bool patch_entry(std::uintptr_t, void*) override { return false; }

  bool restore_entry(std::uintptr_t, const std::uint8_t[5]) override { return false; }
};

neko::reload_session make_session() {
  neko::backend::bundle backends;
  backends.loader = std::make_shared<stub_loader>();
  auto process = std::make_shared<fake_process>();
  backends.symbols = process;
  backends.state = process;
  backends.substituter = std::make_shared<stub_substituter>();
  return neko::reload_session{std::move(backends)};
}

} // namespace

TEST_CASE("watch() without embedded descriptors is a configuration exception") {
  auto session = make_session();
  REQUIRE_THROWS_WITH_AS(static_cast<void>(session.watch()),
                         "reload_session: no embedded reload group descriptors; nothing to watch",
                         std::runtime_error);
}

TEST_CASE("managed group selection rejects unknown ids") {
  auto session = make_session();
  REQUIRE_THROWS_WITH_AS(static_cast<void>(session.watch("physics")),
                         "reload_session: unknown reload group 'physics'", std::runtime_error);
  REQUIRE_THROWS_WITH_AS(static_cast<void>(session.unwatch("physics")),
                         "reload_session: unknown reload group 'physics'", std::runtime_error);
}

TEST_CASE("unwatch() without managed groups is a no-op") {
  auto session = make_session();
  session.unwatch();
  session.unwatch();
  CHECK(session.update().events.empty());
  CHECK(session.snapshot().watched_paths.empty());
}
