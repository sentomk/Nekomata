// Transaction tests for reload_session. These use injected backends so a
// deterministic failure can occur after an earlier object's entry was patched.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <neko/backend.hpp>
#include <neko/backend/code_substituter.hpp>
#include <neko/backend/object_loader.hpp>
#include <neko/backend/state_manager.hpp>
#include <neko/backend/symbol_provider.hpp>
#include <neko/session.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
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

class queued_loader final : public neko::backend::object_loader {
public:
  explicit queued_loader(std::vector<neko::backend::loaded_image> images)
      : images_(std::move(images)) {}

  neko::backend::loaded_image load(const std::uint8_t*, std::size_t) override {
    if (next_ == images_.size()) {
      throw std::runtime_error("unexpected object load");
    }
    return std::move(images_[next_++]);
  }

  std::size_t load_count() const { return next_; }

private:
  std::vector<neko::backend::loaded_image> images_;
  std::size_t next_ = 0;
};

class recording_substituter final : public neko::backend::code_substituter {
public:
  explicit recording_substituter(std::uintptr_t fail_entry, std::uintptr_t fail_restore_entry = 0)
      : fail_entry_(fail_entry), fail_restore_entry_(fail_restore_entry) {}

  neko::backend::executable_allocation_ptr reserve_code_near(std::uintptr_t,
                                                             std::uint64_t) override {
    return nullptr;
  }

  bool commit_code(neko::backend::executable_allocation&, const void*, std::uint64_t) override {
    return false;
  }

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
    return entry != fail_restore_entry_;
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
  std::uintptr_t fail_restore_entry_;
};

struct allocation_counters {
  std::size_t reclaimed = 0;
  std::size_t released_to_process = 0;
};

class fake_allocation final : public neko::backend::executable_allocation {
public:
  fake_allocation(void* data, std::uint64_t size, std::shared_ptr<allocation_counters> counters)
      : data_(data), size_(size), counters_(std::move(counters)) {}

  ~fake_allocation() override {
    if (owns_mapping_) {
      ++counters_->reclaimed;
    }
  }

  void* data() noexcept override { return data_; }
  const void* data() const noexcept override { return data_; }
  std::uint64_t size() const noexcept override { return size_; }

  void release_to_process() noexcept override {
    if (!owns_mapping_) {
      return;
    }
    owns_mapping_ = false;
    ++counters_->released_to_process;
  }

private:
  void* data_;
  std::uint64_t size_;
  std::shared_ptr<allocation_counters> counters_;
  bool owns_mapping_ = true;
};

class fake_writable_allocation final : public neko::backend::writable_allocation {
public:
  fake_writable_allocation(void* data, std::uint64_t size,
                           std::shared_ptr<allocation_counters> counters)
      : data_(data), size_(size), counters_(std::move(counters)) {}

  ~fake_writable_allocation() override {
    if (owns_mapping_) {
      ++counters_->reclaimed;
    }
  }

  void* data() noexcept override { return data_; }
  const void* data() const noexcept override { return data_; }
  std::uint64_t size() const noexcept override { return size_; }

  void release_to_process() noexcept override {
    if (!owns_mapping_) {
      return;
    }
    owns_mapping_ = false;
    ++counters_->released_to_process;
  }

private:
  void* data_;
  std::uint64_t size_;
  std::shared_ptr<allocation_counters> counters_;
  bool owns_mapping_ = true;
};

neko::backend::loaded_image image(void* code, std::string name, std::uintptr_t old_entry,
                                  const std::shared_ptr<allocation_counters>& counters) {
  neko::backend::loaded_image loaded;
  loaded.allocation = std::make_unique<fake_allocation>(code, 8, counters);
  loaded.replacements.push_back({std::move(name), old_entry, 0});
  return loaded;
}

void attach_state(neko::backend::loaded_image& loaded, void* data,
                  const std::shared_ptr<allocation_counters>& counters) {
  loaded.state_allocations.push_back(
      std::make_unique<fake_writable_allocation>(data, sizeof(std::uint64_t), counters));
}

} // namespace

TEST_CASE("one update rolls back entries patched for earlier watched objects") {
  constexpr std::uintptr_t a_entry = 0x1010;
  constexpr std::uintptr_t b_entry = 0x2020;

  std::array<std::uint8_t, 8> a_code{};
  std::array<std::uint8_t, 8> b_code{};
  auto allocations = std::make_shared<allocation_counters>();
  std::vector<neko::backend::loaded_image> images;
  images.push_back(image(a_code.data(), "a_tick", a_entry, allocations));
  images.push_back(image(b_code.data(), "b_tick", b_entry, allocations));

  auto loader = std::make_shared<queued_loader>(std::move(images));
  auto process = std::make_shared<fake_process>();
  auto substituter = std::make_shared<recording_substituter>(b_entry);

  neko::backend::bundle backends;
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

  const auto result = session.update();
  REQUIRE(result.events.size() == 1);
  REQUIRE(result.events.front().status == neko::update_status::rejected);
  REQUIRE(result.events.front().code == neko::reload_error_code::commit_failed);
  const std::string error = result.events.front().message;

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

  const auto stats = session.snapshot();
  CHECK(stats.applied == 0);
  CHECK(stats.rejected == 1);
  CHECK(stats.last_result == error);
  CHECK(allocations->reclaimed == 2);
  CHECK(allocations->released_to_process == 0);
}

TEST_CASE("an incomplete rollback poisons the session and retains candidate code") {
  constexpr std::uintptr_t a_entry = 0x1110;
  constexpr std::uintptr_t b_entry = 0x2220;
  constexpr auto fatal_error =
      "reload session is unusable: an entry patch failed and rollback could not restore every "
      "written entry";

  std::array<std::uint8_t, 8> a_code{};
  std::array<std::uint8_t, 8> b_code{};
  auto allocations = std::make_shared<allocation_counters>();
  std::vector<neko::backend::loaded_image> images;
  images.push_back(image(a_code.data(), "a_tick", a_entry, allocations));
  images.push_back(image(b_code.data(), "b_tick", b_entry, allocations));

  auto loader = std::make_shared<queued_loader>(std::move(images));
  auto process = std::make_shared<fake_process>();
  auto substituter = std::make_shared<recording_substituter>(b_entry, a_entry);

  neko::backend::bundle backends;
  backends.loader = loader;
  backends.symbols = process;
  backends.state = process;
  backends.substituter = substituter;

  temporary_directory temporary;
  const auto a_offer = temporary.path() / "a.new.o";
  const auto b_offer = temporary.path() / "b.new.o";
  offer(a_offer);
  offer(b_offer);

  {
    neko::reload_session session{std::move(backends)};
    session.watch(a_offer);
    session.watch(b_offer);

    CHECK_THROWS_WITH_AS(static_cast<void>(session.update()), fatal_error, std::runtime_error);
    REQUIRE(substituter->patched.size() == 2);
    CHECK(substituter->patched[0] == a_entry);
    CHECK(substituter->patched[1] == b_entry);
    REQUIRE(substituter->restored.size() == 1);
    CHECK(substituter->restored[0] == a_entry);
    CHECK(allocations->reclaimed == 0);
    CHECK(allocations->released_to_process == 0);

    CHECK_THROWS_WITH_AS(static_cast<void>(session.update()), fatal_error, std::runtime_error);
    CHECK_THROWS_WITH_AS(static_cast<void>(session.snapshot()), fatal_error, std::runtime_error);
    CHECK_THROWS_WITH_AS(session.unwatch(), fatal_error, std::runtime_error);
    CHECK(substituter->patched.size() == 2);
  }

  CHECK(allocations->reclaimed == 0);
  CHECK(allocations->released_to_process == 2);
}

TEST_CASE("one update rejects objects that replace the same live entry") {
  constexpr std::uintptr_t shared_entry = 0x3030;

  std::array<std::uint8_t, 8> first_code{};
  std::array<std::uint8_t, 8> second_code{};
  auto allocations = std::make_shared<allocation_counters>();
  std::vector<neko::backend::loaded_image> images;
  images.push_back(image(first_code.data(), "first_tick", shared_entry, allocations));
  images.push_back(image(second_code.data(), "second_tick", shared_entry, allocations));

  auto loader = std::make_shared<queued_loader>(std::move(images));
  auto process = std::make_shared<fake_process>();
  auto substituter = std::make_shared<recording_substituter>(0);

  neko::backend::bundle backends;
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

  const auto conflicting = session.update();
  REQUIRE(conflicting.events.size() == 1);
  CHECK(conflicting.events.front().status == neko::update_status::rejected);
  CHECK(conflicting.events.front().code == neko::reload_error_code::object_rejected);
  CHECK(conflicting.events.front().message ==
        "reload rejected before any write: multiple objects replace entry of second_tick");
  CHECK(substituter->prechecked.size() == 2);
  CHECK(substituter->snapshotted.empty());
  CHECK(substituter->patched.empty());
  CHECK(substituter->restored.empty());

  const auto stats = session.snapshot();
  CHECK(stats.applied == 0);
  CHECK(stats.rejected == 1);
  CHECK(stats.last_result ==
        "reload rejected before any write: multiple objects replace entry of second_tick");
  CHECK(allocations->reclaimed == 2);
  CHECK(allocations->released_to_process == 0);
}

TEST_CASE("rejected candidate state is reclaimed with its code") {
  constexpr std::uintptr_t entry = 0x6060;
  std::array<std::uint8_t, 8> code{};
  std::uint64_t state = 0;
  auto code_allocations = std::make_shared<allocation_counters>();
  auto state_allocations = std::make_shared<allocation_counters>();
  std::vector<neko::backend::loaded_image> images;
  auto loaded = image(code.data(), "tick", entry, code_allocations);
  attach_state(loaded, &state, state_allocations);
  images.push_back(std::move(loaded));

  auto loader = std::make_shared<queued_loader>(std::move(images));
  auto process = std::make_shared<fake_process>();
  auto substituter = std::make_shared<recording_substituter>(entry);
  substituter->expected_prechecks = 1;

  neko::backend::bundle backends;
  backends.loader = loader;
  backends.symbols = process;
  backends.state = process;
  backends.substituter = substituter;

  temporary_directory temporary;
  const auto object = temporary.path() / "rejected.new.o";
  offer(object);

  neko::reload_session session{std::move(backends)};
  session.watch(object);
  const auto result = session.update();
  REQUIRE(result.events.size() == 1);
  CHECK(result.events.front().status == neko::update_status::rejected);
  CHECK(code_allocations->reclaimed == 1);
  CHECK(state_allocations->reclaimed == 1);
  CHECK(state_allocations->released_to_process == 0);
}

TEST_CASE("committed state follows redirects into process lifetime") {
  constexpr std::uintptr_t entry = 0x7070;
  std::array<std::uint8_t, 8> code{};
  std::uint64_t state = 0;
  auto code_allocations = std::make_shared<allocation_counters>();
  auto state_allocations = std::make_shared<allocation_counters>();
  std::vector<neko::backend::loaded_image> images;
  auto loaded = image(code.data(), "tick", entry, code_allocations);
  attach_state(loaded, &state, state_allocations);
  images.push_back(std::move(loaded));

  auto loader = std::make_shared<queued_loader>(std::move(images));
  auto process = std::make_shared<fake_process>();
  auto substituter = std::make_shared<recording_substituter>(0);
  substituter->expected_prechecks = 1;

  neko::backend::bundle backends;
  backends.loader = loader;
  backends.symbols = process;
  backends.state = process;
  backends.substituter = substituter;

  temporary_directory temporary;
  const auto object = temporary.path() / "committed.new.o";
  offer(object);
  {
    neko::reload_session session{std::move(backends)};
    session.watch(object);
    CHECK(session.update().any_applied());
    CHECK(state_allocations->reclaimed == 0);
    CHECK(state_allocations->released_to_process == 0);
  }
  CHECK(state_allocations->reclaimed == 0);
  CHECK(state_allocations->released_to_process == 1);
}
