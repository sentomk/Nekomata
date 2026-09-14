// Unit tests for the immutable, multi-consumer generation stream reader.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "runtime/generation_stream.hpp"
#include "runtime/sha256.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

class temporary_directory {
public:
  temporary_directory() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("neko-generation-stream-" + std::to_string(nonce));
    if (!std::filesystem::create_directory(path_)) {
      throw std::runtime_error("cannot create generation stream test directory");
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

neko::detail::group_descriptor make_descriptor() {
  neko::detail::group_descriptor value;
  value.group_id = "//gameplay:hot";
  value.members = {"gameplay/ball", "gameplay/gravity"};
  value.publication_key = "gameplay-6f10e51d";
  value.baseline_sequence = 42;
  value.compatibility_id = "sha256:compile-identity";
  value.abi_id = "elf-x86_64-patch-v1";
  return value;
}

neko::detail::generation_offer make_offer(std::uint64_t sequence, std::string generation_id = "") {
  neko::detail::generation_offer value;
  value.group_id = "//gameplay:hot";
  value.sequence = sequence;
  value.generation_id = generation_id.empty() ? "gen-" + std::to_string(sequence) : generation_id;
  value.compatibility_id = "sha256:compile-identity";
  value.abi_id = "elf-x86_64-patch-v1";
  value.members = {
      {"gameplay/ball", "objects/ball.o", "", "//src:ball.cpp", "-O0"},
      {"gameplay/gravity", "objects/gravity.o", "", "//src:gravity.cpp", "-O0"},
  };
  value.changed_inputs = {"//src:ball.cpp"};
  return value;
}

std::string object_bytes(const std::string& member_key, std::uint64_t sequence) {
  return "object:" + member_key + ":" + std::to_string(sequence);
}

class stream_fixture {
public:
  stream_fixture() : descriptor_(make_descriptor()) {
    publication_ = temporary_.path() / descriptor_.publication_key;
    std::filesystem::create_directories(publication_ / "offers");
  }

  const std::filesystem::path& root() const { return temporary_.path(); }
  const neko::detail::group_descriptor& descriptor() const { return descriptor_; }
  const std::filesystem::path& publication() const { return publication_; }

  neko::detail::generation_stream make_stream() const {
    return neko::detail::generation_stream{descriptor_, root()};
  }

  /// Materializes and publishes one complete generation. Digests are computed
  /// from the written bytes unless `poison_first_digest` reserves a wrong but
  /// well-formed value for the first member.
  void publish(neko::detail::generation_offer offer, bool poison_first_digest = false) {
    const auto directory = publication_ / "generations" / offer.generation_id;
    for (auto& member : offer.members) {
      if (member.sha256.empty()) {
        member.sha256 = neko::detail::sha256_hex(object_bytes(member.member, offer.sequence));
      }
      write_bytes(directory / std::filesystem::path(member.object_path),
                  object_bytes(member.member, offer.sequence));
    }
    if (poison_first_digest) {
      offer.members.front().sha256 = std::string(64, 'e');
    }
    write_bytes(directory / "manifest", neko::detail::serialize_generation_offer(offer));
    write_bytes(
        publication_ / "offers" /
            neko::detail::serialize_generation_offer_marker({offer.sequence, offer.generation_id}),
        "");
  }

  void write_bytes(const std::filesystem::path& path, std::string_view bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!output) {
      throw std::runtime_error("cannot create generation stream fixture file");
    }
    published_.emplace(path, std::string(bytes));
  }

  /// Every published file and its exact content, for untouched-by-consumption
  /// assertions.
  const std::map<std::filesystem::path, std::string>& published() const { return published_; }

private:
  temporary_directory temporary_;
  neko::detail::group_descriptor descriptor_;
  std::filesystem::path publication_;
  std::map<std::filesystem::path, std::string> published_;
};

} // namespace

TEST_CASE("an absent stream stays idle and baseline offers are skipped") {
  stream_fixture fixture;
  auto stream = fixture.make_stream();
  REQUIRE(stream.cursor() == 42);

  CHECK(stream.poll().status == neko::detail::stream_status::idle);

  fixture.publish(make_offer(41));
  fixture.publish(make_offer(42)); // equal to the baseline: already in the executable
  CHECK(stream.poll().status == neko::detail::stream_status::idle);
  CHECK(stream.cursor() == 42);
}

TEST_CASE("the newest offer wins and equal sequences tie-break by generation id") {
  stream_fixture fixture;
  fixture.publish(make_offer(43, "gen-a"));
  fixture.publish(make_offer(45, "gen-b"));
  fixture.publish(make_offer(44));

  auto stream = fixture.make_stream();
  const auto observation = stream.poll();
  REQUIRE(observation.status == neko::detail::stream_status::consumed);
  CHECK(observation.sequence == 45);
  CHECK(observation.generation_id == "gen-b");
  CHECK(stream.cursor() == 45);

  stream_fixture tied;
  tied.publish(make_offer(46, "gen-a"));
  tied.publish(make_offer(46, "gen-b"));
  const auto tied_observation = tied.make_stream().poll();
  REQUIRE(tied_observation.status == neko::detail::stream_status::consumed);
  CHECK(tied_observation.sequence == 46);
  CHECK(tied_observation.generation_id == "gen-b");
}

TEST_CASE("a complete generation is consumed with digest-verified objects") {
  stream_fixture fixture;
  fixture.publish(make_offer(43));

  auto stream = fixture.make_stream();
  const auto observation = stream.poll();
  REQUIRE(observation.status == neko::detail::stream_status::consumed);
  CHECK(observation.offer.group_id == "//gameplay:hot");
  CHECK(observation.offer.sequence == 43);
  CHECK(observation.offer.members.size() == 2);
  REQUIRE(observation.objects.size() == 2);
  CHECK(observation.objects.front().filename() == "ball.o");
  CHECK(observation.objects.back().filename() == "gravity.o");
  CHECK(observation.generation_directory == fixture.publication() / "generations" / "gen-43");
  CHECK(stream.cursor() == 43);
  CHECK(stream.poll().status == neko::detail::stream_status::idle);
}

TEST_CASE("markers must agree with their manifest") {
  stream_fixture fixture;
  fixture.publish(make_offer(43));
  fixture.write_bytes(fixture.publication() / "offers" / "44-gen-43.ready", "");

  auto stream = fixture.make_stream();
  const auto observation = stream.poll();
  REQUIRE(observation.status == neko::detail::stream_status::rejected);
  CHECK(observation.sequence == 44);
  CHECK(observation.message ==
        "invalid generation offer '44-gen-43.ready': marker sequence 44 does not match manifest "
        "sequence 43");
  CHECK(stream.cursor() == 44);
  CHECK(stream.poll().status == neko::detail::stream_status::idle);
}

TEST_CASE("membership against the descriptor is exact and ordered") {
  stream_fixture fixture;
  auto swapped = make_offer(43);
  std::swap(swapped.members.front(), swapped.members.back());
  fixture.publish(swapped);

  auto stream = fixture.make_stream();
  auto observation = stream.poll();
  REQUIRE(observation.status == neko::detail::stream_status::rejected);
  CHECK(observation.message ==
        "invalid generation offer '" +
            (fixture.publication() / "generations" / "gen-43" / "manifest").generic_string() +
            "': member at index 0 is 'gameplay/gravity', expected 'gameplay/ball'");

  stream_fixture missing;
  auto incomplete = make_offer(43);
  incomplete.members.pop_back();
  missing.publish(incomplete);
  observation = missing.make_stream().poll();
  REQUIRE(observation.status == neko::detail::stream_status::rejected);
  CHECK(observation.message ==
        "invalid generation offer '" +
            (missing.publication() / "generations" / "gen-43" / "manifest").generic_string() +
            "': member count 1 does not match descriptor count 2");

  stream_fixture extra;
  auto inflated = make_offer(43);
  inflated.members.push_back({"gameplay/other", "objects/other.o", "", "//src:other.cpp", "-O0"});
  extra.publish(inflated);
  observation = extra.make_stream().poll();
  REQUIRE(observation.status == neko::detail::stream_status::rejected);
  CHECK(observation.message ==
        "invalid generation offer '" +
            (extra.publication() / "generations" / "gen-43" / "manifest").generic_string() +
            "': member count 3 does not match descriptor count 2");
}

TEST_CASE("object paths may not escape the generation directory") {
  stream_fixture fixture;
  fixture.publish(make_offer(43));

  const auto generation = fixture.publication() / "generations" / "gen-43";
  const auto escaped = fixture.root() / "escaped.o";
  fixture.write_bytes(escaped, "outside the generation directory");
  std::filesystem::remove(generation / "objects" / "ball.o");
  std::filesystem::create_symlink(escaped, generation / "objects" / "ball.o");

  auto stream = fixture.make_stream();
  const auto observation = stream.poll();
  REQUIRE(observation.status == neko::detail::stream_status::rejected);
  CHECK(observation.message ==
        "generation stream 'gameplay-6f10e51d': member 'gameplay/ball' object 'objects/ball.o' "
        "resolves outside the generation directory");
}

TEST_CASE("object digests and presence are checked") {
  stream_fixture fixture;
  fixture.publish(make_offer(43), /*poison_first_digest=*/true);

  auto stream = fixture.make_stream();
  auto observation = stream.poll();
  REQUIRE(observation.status == neko::detail::stream_status::rejected);
  CHECK(observation.message ==
        "generation stream 'gameplay-6f10e51d': member 'gameplay/ball' object 'objects/ball.o' "
        "digest mismatch: expected '" +
            std::string(64, 'e') + "', computed '" +
            neko::detail::sha256_hex(object_bytes("gameplay/ball", 43)) + "'");

  stream_fixture missing;
  missing.publish(make_offer(43));
  std::filesystem::remove(missing.publication() / "generations" / "gen-43" / "objects" /
                          "gravity.o");
  observation = missing.make_stream().poll();
  REQUIRE(observation.status == neko::detail::stream_status::rejected);
  CHECK(observation.message ==
        "generation stream 'gameplay-6f10e51d': member 'gameplay/gravity' object "
        "'objects/gravity.o' is missing or not a regular file");
}

TEST_CASE("a rejected offer advances the cursor once and a later generation recovers") {
  stream_fixture fixture;
  fixture.publish(make_offer(43), /*poison_first_digest=*/true);

  auto stream = fixture.make_stream();
  auto observation = stream.poll();
  REQUIRE(observation.status == neko::detail::stream_status::rejected);
  CHECK(observation.sequence == 43);
  CHECK(stream.cursor() == 43);
  CHECK(stream.poll().status == neko::detail::stream_status::idle); // reported once

  fixture.write_bytes(fixture.publication() / "offers" / "50-gen-50.ready", "");
  observation = stream.poll();
  REQUIRE(observation.status == neko::detail::stream_status::rejected);
  CHECK(observation.sequence == 50);
  CHECK(observation.message ==
        "generation stream 'gameplay-6f10e51d': generation 'gen-50' has no readable manifest "
        "at '" +
            (fixture.publication() / "generations" / "gen-50" / "manifest").generic_string() + "'");

  fixture.publish(make_offer(51));
  observation = stream.poll();
  REQUIRE(observation.status == neko::detail::stream_status::consumed);
  CHECK(observation.sequence == 51);
  CHECK(stream.cursor() == 51);
}

TEST_CASE("malformed marker filenames are reported once without moving the cursor") {
  stream_fixture fixture;
  fixture.write_bytes(fixture.publication() / "offers" / "broken.ready", "");

  auto stream = fixture.make_stream();
  auto observation = stream.poll();
  REQUIRE(observation.status == neko::detail::stream_status::rejected);
  CHECK(observation.sequence == 0);
  CHECK(observation.message ==
        "generation stream 'gameplay-6f10e51d': offer marker 'broken.ready' is not a valid "
        "offer marker");
  CHECK(stream.cursor() == 42);
  CHECK(stream.poll().status == neko::detail::stream_status::idle); // once only

  fixture.publish(make_offer(43));
  observation = stream.poll();
  REQUIRE(observation.status == neko::detail::stream_status::consumed);
  CHECK(observation.sequence == 43);
}

TEST_CASE("consumption never modifies published files") {
  stream_fixture fixture;
  fixture.publish(make_offer(43));
  fixture.publish(make_offer(44));

  auto stream = fixture.make_stream();
  REQUIRE(stream.poll().status == neko::detail::stream_status::consumed);

  for (const auto& [path, bytes] : fixture.published()) {
    INFO(path.generic_string());
    REQUIRE(std::filesystem::exists(path));
    std::ifstream input(path, std::ios::binary);
    const std::string content{std::istreambuf_iterator<char>(input),
                              std::istreambuf_iterator<char>()};
    CHECK(content == bytes);
  }
}

TEST_CASE("two consumers independently read the same offers") {
  stream_fixture fixture;
  fixture.publish(make_offer(43));

  auto first = fixture.make_stream();
  auto second = fixture.make_stream();
  auto first_observation = first.poll();
  auto second_observation = second.poll();
  REQUIRE(first_observation.status == neko::detail::stream_status::consumed);
  REQUIRE(second_observation.status == neko::detail::stream_status::consumed);
  CHECK(first_observation.sequence == second_observation.sequence);
  CHECK(first_observation.generation_id == second_observation.generation_id);
  CHECK(first.cursor() == second.cursor());
  CHECK(first.poll().status == neko::detail::stream_status::idle);
  CHECK(second.poll().status == neko::detail::stream_status::idle);

  fixture.publish(make_offer(44));
  CHECK(first.poll().sequence == 44);
  CHECK(second.poll().sequence == 44);
}
