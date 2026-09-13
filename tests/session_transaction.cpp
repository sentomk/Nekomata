// Transaction tests for reload_session. These use injected backends so a
// deterministic failure can occur after an earlier object's entry was patched.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <neko/runtime/code_substituter.hpp>
#include <neko/runtime/object_loader.hpp>
#include <neko/runtime/state_manager.hpp>
#include <neko/runtime/symbol_provider.hpp>
#include <neko/session.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

class temporary_directory {
public:
  temporary_directory() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("neko-session-transaction-" + std::to_string(nonce));
    if (!std::filesystem::create_directory(path_)) {
      throw std::runtime_error("cannot create transaction test directory");
    }
  }

  ~temporary_directory() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  const std::filesystem::path& path() const { return path_; }

private:
  std::filesystem::path path_;
};

void offer(const std::filesystem::path& path) {
  std::ofstream file(path, std::ios::binary);
  file.put('\0');
  if (!file) {
    throw std::runtime_error("cannot create transaction test offer");
  }
}

class fake_process final : public neko::symbol_provider, public neko::state_manager {
public:
  std::vector<neko::function_info> all_functions() const override { return {}; }

  std::optional<neko::function_info> function_by_name(std::string_view) const override {
    return std::nullopt;
  }

  std::size_t count_functions(std::string_view) const override { return 0; }

  std::optional<neko::global_variable> global_by_name(std::string_view) const override {
    return std::nullopt;
  }

  std::size_t count_globals(std::string_view) const override { return 0; }

  neko::type_layout layout_of(neko::type_id id) const override {
    neko::type_layout layout;
    layout.id = id;
    return layout;
  }

  void* map_global(std::string_view) override { return nullptr; }
};

class queued_loader final : public neko::object_loader {
public:
  explicit queued_loader(std::vector<neko::loaded_image> images) : images_(std::move(images)) {}

  neko::loaded_image load(const std::uint8_t*, std::size_t) override {
    if (next_ == images_.size()) {
      throw std::runtime_error("unexpected object load");
    }
    return std::move(images_[next_++]);
  }

private:
  std::vector<neko::loaded_image> images_;
  std::size_t next_ = 0;
};

class recording_substituter final : public neko::code_substituter {
public:
  explicit recording_substituter(std::uintptr_t fail_entry) : fail_entry_(fail_entry) {}

  void* reserve_code_near(std::uintptr_t, std::uint64_t) override { return nullptr; }

  bool commit_code(void*, const void*, std::uint64_t) override { return false; }

  bool precheck_entry(std::uintptr_t entry, void*) override {
    prechecked.push_back(entry);
    return true;
  }

  bool snapshot_entry(std::uintptr_t entry, std::uint8_t out[5]) override {
    if (prechecked.size() != expected_prechecks) {
      patching_started_before_preparation_finished = true;
    }
    std::fill_n(out, 5, std::uint8_t{0});
    snapshotted.push_back(entry);
    return true;
  }

  bool patch_entry(std::uintptr_t entry, void*) override {
    if (snapshotted.size() != expected_prechecks) {
      patching_started_before_snapshots_finished = true;
    }
    patched.push_back(entry);
    return entry != fail_entry_;
  }

  bool restore_entry(std::uintptr_t entry, const std::uint8_t[5]) override {
    restored.push_back(entry);
    return true;
  }

  std::size_t expected_prechecks = 2;
  bool patching_started_before_preparation_finished = false;
  bool patching_started_before_snapshots_finished = false;
  std::vector<std::uintptr_t> prechecked;
  std::vector<std::uintptr_t> snapshotted;
  std::vector<std::uintptr_t> patched;
  std::vector<std::uintptr_t> restored;

private:
  std::uintptr_t fail_entry_;
};

neko::loaded_image image(void* code, std::string name, std::uintptr_t old_entry) {
  neko::loaded_image loaded;
  loaded.code = code;
  loaded.code_size = 8;
  loaded.replacements.push_back({std::move(name), old_entry, 0});
  return loaded;
}

} // namespace

TEST_CASE("one update rolls back entries patched for earlier watched objects") {
  constexpr std::uintptr_t a_entry = 0x1010;
  constexpr std::uintptr_t b_entry = 0x2020;

  std::array<std::uint8_t, 8> a_code{};
  std::array<std::uint8_t, 8> b_code{};
  std::vector<neko::loaded_image> images;
  images.push_back(image(a_code.data(), "a_tick", a_entry));
  images.push_back(image(b_code.data(), "b_tick", b_entry));

  auto loader = std::make_shared<queued_loader>(std::move(images));
  auto process = std::make_shared<fake_process>();
  auto substituter = std::make_shared<recording_substituter>(b_entry);

  neko::backend_bundle backends;
  backends.loader = loader;
  backends.symbols = process;
  backends.state = process;
  backends.substituter = substituter;

  temporary_directory temporary;
  const auto a_offer = temporary.path() / "a.new.o";
  const auto b_offer = temporary.path() / "b.new.o";
  offer(a_offer);
  offer(b_offer);

  neko::reload_session session{std::move(backends)};
  session.watch(a_offer);
  session.watch(b_offer);

  std::string error;
  try {
    static_cast<void>(session.update());
    FAIL("the injected second patch failure must reject the transaction");
  } catch (const std::runtime_error& exception) {
    error = exception.what();
  }

  CHECK(error == "reload rejected and rolled back 1 entry: cannot patch entry of b_tick");
  CHECK_FALSE(substituter->patching_started_before_preparation_finished);
  CHECK_FALSE(substituter->patching_started_before_snapshots_finished);
  REQUIRE(substituter->prechecked.size() == 2);
  CHECK(substituter->prechecked[0] == a_entry);
  CHECK(substituter->prechecked[1] == b_entry);
  REQUIRE(substituter->patched.size() == 2);
  CHECK(substituter->patched[0] == a_entry);
  CHECK(substituter->patched[1] == b_entry);
  REQUIRE(substituter->restored.size() == 1);
  CHECK(substituter->restored[0] == a_entry);

  const auto stats = session.session_stats();
  CHECK(stats.applied == 0);
  CHECK(stats.rejected == 1);
  CHECK(stats.last_result == error);
}

TEST_CASE("one update rejects objects that replace the same live entry") {
  constexpr std::uintptr_t shared_entry = 0x3030;

  std::array<std::uint8_t, 8> first_code{};
  std::array<std::uint8_t, 8> second_code{};
  std::vector<neko::loaded_image> images;
  images.push_back(image(first_code.data(), "first_tick", shared_entry));
  images.push_back(image(second_code.data(), "second_tick", shared_entry));

  auto loader = std::make_shared<queued_loader>(std::move(images));
  auto process = std::make_shared<fake_process>();
  auto substituter = std::make_shared<recording_substituter>(0);

  neko::backend_bundle backends;
  backends.loader = loader;
  backends.symbols = process;
  backends.state = process;
  backends.substituter = substituter;

  temporary_directory temporary;
  const auto first_offer = temporary.path() / "first.new.o";
  const auto second_offer = temporary.path() / "second.new.o";
  offer(first_offer);
  offer(second_offer);

  neko::reload_session session{std::move(backends)};
  session.watch(first_offer);
  session.watch(second_offer);

  CHECK_THROWS_WITH_AS(
      static_cast<void>(session.update()),
      "reload rejected before any write: multiple objects replace entry of second_tick",
      std::runtime_error);
  CHECK(substituter->prechecked.size() == 2);
  CHECK(substituter->snapshotted.empty());
  CHECK(substituter->patched.empty());
  CHECK(substituter->restored.empty());

  const auto stats = session.session_stats();
  CHECK(stats.applied == 0);
  CHECK(stats.rejected == 1);
  CHECK(stats.last_result ==
        "reload rejected before any write: multiple objects replace entry of second_tick");
}
