// Exercise the public managed-session contract with real publication files
// and the preparation worker. Only discovery and machine-code backends are fake.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <base/sha256.hpp>
#include <neko/backend.hpp>
#include <neko/session.hpp>
#include <protocol/generation_offer.hpp>
#include <runtime/descriptor_discovery.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

std::vector<neko::detail::group_descriptor> registered_groups;

class test_image final : public neko::backend::executable_allocation {
public:
  explicit test_image(std::uint8_t version) : bytes_{version} {}
  void* data() noexcept override { return bytes_.data(); }
  const void* data() const noexcept override { return bytes_.data(); }
  std::uint64_t size() const noexcept override { return bytes_.size(); }
  // No real redirects outlive this fixture; its substituter copies the version.
  void release_to_process() noexcept override {}

private:
  std::array<std::uint8_t, 1> bytes_;
};

class test_backend final : public neko::backend::object_loader,
                           public neko::backend::symbol_provider,
                           public neko::backend::state_manager,
                           public neko::backend::code_substituter {
public:
  std::atomic<unsigned> loads{0};
  unsigned active_version = 0;
  unsigned patches = 0;

  neko::backend::loaded_image load(const std::uint8_t* bytes, std::size_t size) override {
    ++loads;
    if (size != 1 || bytes[0] == 255) {
      throw std::runtime_error("fixture rejected object");
    }
    neko::backend::loaded_image image;
    image.allocation = std::make_unique<test_image>(bytes[0]);
    image.replacements.push_back({"tick", 1, 0});
    return image;
  }
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
  neko::backend::executable_allocation_ptr reserve_code_near(std::uintptr_t,
                                                             std::uint64_t) override {
    return nullptr;
  }
  bool commit_code(neko::backend::executable_allocation&, const void*, std::uint64_t) override {
    return false;
  }
  bool precheck_entry(std::uintptr_t, void*) override { return true; }
  bool snapshot_entry(std::uintptr_t, std::uint8_t out[5]) override {
    out[0] = static_cast<std::uint8_t>(active_version);
    return true;
  }
  bool patch_entry(std::uintptr_t, void* target) override {
    active_version = *static_cast<std::uint8_t*>(target);
    ++patches;
    return true;
  }
  bool restore_entry(std::uintptr_t, const std::uint8_t original[5]) override {
    active_version = original[0];
    return true;
  }
};

void write_file(const std::filesystem::path& path, const std::string& bytes) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary);
  output << bytes;
  output.close();
  if (!output) {
    throw std::runtime_error("cannot write lifecycle fixture");
  }
}

class fixture {
public:
  fixture() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    root = std::filesystem::temp_directory_path() / ("neko-lifecycle-" + std::to_string(nonce));
    if (!std::filesystem::create_directory(root)) {
      throw std::runtime_error("cannot create lifecycle directory");
    }
    registered_groups.clear();
    for (const auto* id : {"alpha", "beta"}) {
      neko::detail::group_descriptor descriptor;
      descriptor.group_id = id;
      descriptor.members = {"hot"};
      descriptor.publication_key = id;
      descriptor.compatibility_id = "fixture-build";
      descriptor.abi_id = "fixture-abi";
      descriptor.generation_root_hint = root.generic_string();
      registered_groups.push_back(std::move(descriptor));
    }
    neko::backend::bundle bundle;
    bundle.loader = backend;
    bundle.symbols = backend;
    bundle.state = backend;
    bundle.substituter = backend;
    session = std::make_unique<neko::reload_session>(std::move(bundle));
  }
  ~fixture() {
    session.reset(); // Join the worker before deleting its input files.
    registered_groups.clear();
    std::error_code error;
    std::filesystem::remove_all(root, error);
  }

  void publish(const std::string& group, unsigned sequence, std::uint8_t version) const {
    neko::detail::generation_offer offer;
    offer.group_id = group;
    offer.sequence = sequence;
    offer.generation_id = "gen-" + std::to_string(sequence);
    offer.compatibility_id = "fixture-build";
    offer.abi_id = "fixture-abi";
    const std::string bytes(1, static_cast<char>(version));
    offer.members = {{"hot", "objects/hot.o", neko::detail::sha256_hex(bytes), "hot.cpp", "-O0"}};
    const auto directory = root / group / "generations" / offer.generation_id;
    write_file(directory / "objects/hot.o", bytes);
    write_file(directory / "manifest", neko::detail::serialize_generation_offer(offer));
    const auto marker =
        root / group / "offers" /
        neko::detail::serialize_generation_offer_marker({offer.sequence, offer.generation_id});
    write_file(marker.string() + ".staging", "");
    std::filesystem::rename(marker.string() + ".staging", marker);
  }

  neko::group_snapshot group(const std::string& id = "alpha") const {
    for (const auto& snapshot : session->snapshot().managed_groups) {
      if (snapshot.group_id == id) {
        return snapshot;
      }
    }
    throw std::runtime_error("missing fixture group");
  }

  bool await(unsigned sequence, neko::group_state state, const std::string& id = "alpha") const {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do {
      const auto snapshot = group(id);
      if (snapshot.observed_sequence == sequence && snapshot.state == state) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
  }

  std::filesystem::path root;
  std::shared_ptr<test_backend> backend = std::make_shared<test_backend>();
  std::unique_ptr<neko::reload_session> session;
};

} // namespace

namespace neko::detail {

// This link-time substitute only supplies the construction-time registry.
// Real linked-section discovery has its own integration and ELF reload suites.
std::vector<group_descriptor> discover_embedded_descriptors() {
  return registered_groups;
}

} // namespace neko::detail

TEST_CASE("managed groups start disabled and watch selection is idempotent") {
  fixture test;
  test.publish("alpha", 1, 11);
  CHECK_FALSE(test.group().enabled);
  CHECK_FALSE(test.group("beta").enabled);
  CHECK(test.session->update().events.empty());
  CHECK(test.backend->loads == 0);
  test.session->watch("alpha");
  test.session->watch("alpha");
  CHECK(test.group().enabled);
  CHECK_FALSE(test.group("beta").enabled);
  test.session->watch();
  test.session->watch();
  CHECK(test.group("beta").enabled);
  test.session->unwatch("alpha");
  test.session->unwatch("alpha");
  CHECK_FALSE(test.group().enabled);
  CHECK(test.group("beta").enabled);
  test.session->unwatch();
  test.session->unwatch();
  CHECK_FALSE(test.group("beta").enabled);
  CHECK(test.session->update().events.empty());
}

TEST_CASE("unwatch retains ready work and the cursor until a resumed safe point") {
  fixture test;
  bool all_groups = false;
  SUBCASE("named group") {}
  SUBCASE("all groups") {
    all_groups = true;
  }
  const auto watch = [&] {
    if (all_groups) {
      test.session->watch();
    } else {
      test.session->watch("alpha");
    }
  };
  const auto unwatch = [&] {
    if (all_groups) {
      test.session->unwatch();
    } else {
      test.session->unwatch("alpha");
    }
  };
  test.publish("alpha", 1, 11);
  watch();
  REQUIRE(test.await(1, neko::group_state::ready));
  CHECK(test.backend->patches == 0); // Preparation alone cannot activate code.
  unwatch();
  unwatch();
  CHECK_FALSE(test.group().enabled);
  CHECK(test.group().state == neko::group_state::ready);
  CHECK(test.group().observed_sequence == 1);
  CHECK(test.session->update().events.empty());
  CHECK(test.backend->patches == 0);

  // Retained work must apply without re-reading the already-observed artifact.
  std::filesystem::remove(test.root / "alpha/generations/gen-1/objects/hot.o");
  watch();
  watch();
  CHECK(test.backend->patches == 0);
  const auto applied = test.session->update();
  REQUIRE(applied.events.size() == 1);
  CHECK(applied.events[0].status == neko::update_status::applied);
  CHECK(applied.events[0].generation_id == "gen-1");
  CHECK(applied.events[0].group_id == "alpha");
  CHECK(applied.events[0].redirected_function_count == 1);
  CHECK(test.backend->active_version == 11);
  CHECK(test.backend->loads == 1);
  CHECK(test.session->update().events.empty());

  unwatch();
  test.publish("alpha", 2, 22);
  CHECK(test.session->update().events.empty());
  CHECK(test.group().observed_sequence == 1);
  CHECK(test.backend->active_version == 11);
  watch();
  REQUIRE(test.await(2, neko::group_state::ready));
  CHECK(test.backend->active_version == 11);
  const auto resumed = test.session->update();
  REQUIRE(resumed.events.size() == 1);
  CHECK(resumed.events[0].generation_id == "gen-2");
  CHECK(resumed.events[0].status == neko::update_status::applied);
  CHECK(test.backend->active_version == 22);
  CHECK(test.backend->loads == 2);
  CHECK(test.backend->patches == 2);
  CHECK(test.session->snapshot().applied == 2);
}

TEST_CASE("resume does not prepare an already consumed generation again") {
  fixture test;
  test.publish("alpha", 1, 11);
  test.session->watch("alpha");
  REQUIRE(test.await(1, neko::group_state::ready));
  REQUIRE(test.session->update().any_applied());
  test.session->unwatch();
  test.session->watch();
  // The worker visits alpha before beta. Observing beta proves a resumed
  // observation pass has run, without guessing how long that pass takes.
  test.publish("beta", 1, 22);
  REQUIRE(test.await(1, neko::group_state::ready, "beta"));
  CHECK(test.group().observed_sequence == 1);
  CHECK(test.group().last_applied_generation == "gen-1");
  const auto result = test.session->update();
  REQUIRE(result.events.size() == 1);
  CHECK(result.events[0].group_id == "beta");
  CHECK(test.backend->loads == 2);
  CHECK(test.backend->patches == 2);
}

TEST_CASE("unwatch retains a pending rejection and reports it once after resume") {
  neko::update_event expected;
  {
    fixture baseline;
    baseline.publish("alpha", 1, 255);
    baseline.session->watch("alpha");
    REQUIRE(baseline.await(1, neko::group_state::failed));
    const auto result = baseline.session->update();
    REQUIRE(result.events.size() == 1);
    expected = result.events[0];
    REQUIRE(expected.status == neko::update_status::rejected);
    REQUIRE(expected.code != neko::reload_error_code::none);
  }
  fixture test;
  test.publish("alpha", 1, 255);
  test.session->watch("alpha");
  REQUIRE(test.await(1, neko::group_state::failed));
  test.session->unwatch("alpha");
  CHECK(test.session->update().events.empty());
  CHECK(test.session->snapshot().rejected == 0);
  CHECK(test.group().observed_sequence == 1);
  test.session->watch("alpha");
  const auto rejected = test.session->update();
  REQUIRE(rejected.events.size() == 1);
  CHECK(rejected.events[0].status == neko::update_status::rejected);
  CHECK(rejected.events[0].code == expected.code);
  CHECK(rejected.events[0].group_id == expected.group_id);
  CHECK(rejected.events[0].message == "fixture rejected object");
  CHECK(rejected.events[0].generation_id == "gen-1");
  CHECK(test.session->update().events.empty());
  CHECK(test.session->snapshot().rejected == 1);
  CHECK(test.backend->loads == 1);
  CHECK(test.backend->patches == 0);
}

TEST_CASE("disabling one ready group does not prevent another group from applying") {
  fixture test;
  test.publish("alpha", 1, 11);
  test.publish("beta", 1, 22);
  test.session->watch();
  REQUIRE(test.await(1, neko::group_state::ready));
  REQUIRE(test.await(1, neko::group_state::ready, "beta"));
  test.session->unwatch("alpha");
  test.publish("alpha", 2, 33);
  test.publish("beta", 2, 44);
  // Beta's progress proves observation ran while alpha was disabled.
  REQUIRE(test.await(2, neko::group_state::ready, "beta"));
  CHECK(test.group().observed_sequence == 1);
  const auto result = test.session->update();
  REQUIRE(result.events.size() == 1);
  CHECK(result.events[0].group_id == "beta");
  CHECK(result.events[0].generation_id == "gen-2");
  CHECK(result.events[0].status == neko::update_status::applied);
  CHECK(test.group().state == neko::group_state::ready);
  test.session->watch("alpha");
  const auto resumed = test.session->update();
  REQUIRE(resumed.events.size() == 1);
  CHECK(resumed.events[0].group_id == "alpha");
  CHECK(resumed.events[0].status == neko::update_status::applied);
}
