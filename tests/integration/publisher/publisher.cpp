// Tests for the shared host publication primitive: the consumer
// (`generation_stream`) must accept exactly what the publisher releases,
// sequences must serialize under concurrency, and identical content must
// reuse its generation identity.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "runtime/generation_stream.hpp"
#include "runtime/publisher.hpp"
#include <base/sha256.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

class temporary_directory {
public:
  temporary_directory() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() / ("neko-publisher-" + std::to_string(nonce));
    if (!std::filesystem::create_directory(path_)) {
      throw std::runtime_error("cannot create publisher test directory");
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
  value.group_id = "//pub:hot";
  value.members = {"pub/a", "pub/b"};
  value.publication_key = "pub-e2e";
  value.baseline_sequence = 0;
  value.compatibility_id = "sha256:compile-identity";
  value.abi_id = "elf-x86_64-patch-v1";
  return value;
}

void write_object(const std::filesystem::path& path, std::string_view bytes) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary);
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (!output) {
    throw std::runtime_error("cannot write publisher test object");
  }
}

// Writes both member objects and publishes one generation of the group.
neko::detail::publish_result publish(const std::filesystem::path& root, const std::string& a_bytes,
                                     const std::string& b_bytes) {
  write_object(root / "in/a.o", a_bytes);
  write_object(root / "in/b.o", b_bytes);
  neko::detail::publish_request request;
  request.generation_root = root;
  request.publication_key = "pub-e2e";
  request.group_id = "//pub:hot";
  request.compatibility_id = "sha256:compile-identity";
  request.abi_id = "elf-x86_64-patch-v1";
  request.members = {
      {"pub/a", root / "in/a.o", "//pub:a.cpp", "-O0"},
      {"pub/b", root / "in/b.o", "//pub:b.cpp", "-O0"},
  };
  request.changed_inputs = {"//pub:a.cpp"};
  return neko::detail::publish_generation(request);
}

} // namespace

TEST_CASE("a published generation is consumed exactly as offered") {
  temporary_directory temporary;
  const auto root = temporary.path();

  const auto first = publish(root, "object-a-v1", "object-b-v1");
  CHECK(first.sequence == 1);
  CHECK(first.generation_id.rfind("g-", 0) == 0);

  neko::detail::generation_stream stream{make_descriptor(), root};
  const auto observation = stream.poll();
  REQUIRE(observation.status == neko::detail::stream_status::consumed);
  CHECK(observation.sequence == 1);
  CHECK(observation.generation_id == first.generation_id);
  REQUIRE(observation.offer.members.size() == 2);
  CHECK(observation.offer.members.front().sha256 == neko::detail::sha256_hex("object-a-v1"));
  CHECK(observation.offer.members.back().sha256 == neko::detail::sha256_hex("object-b-v1"));
  REQUIRE(observation.objects.size() == 2);
  CHECK(observation.objects.front().filename() == "a.o");
  CHECK(observation.objects.back().filename() == "b.o");
  CHECK(stream.cursor() == 1);
  CHECK(stream.poll().status == neko::detail::stream_status::idle);
}

TEST_CASE("sequences increase and identical content reuses its identity") {
  temporary_directory temporary;
  const auto root = temporary.path();

  const auto first = publish(root, "same-a", "same-b");
  const auto again = publish(root, "same-a", "same-b");
  CHECK(first.sequence == 1);
  CHECK(again.sequence == 2);
  CHECK(again.generation_id == first.generation_id);

  const auto changed = publish(root, "same-a", "different-b");
  CHECK(changed.sequence == 3);
  CHECK(changed.generation_id != first.generation_id);

  neko::detail::generation_stream stream{make_descriptor(), root};
  const auto observation = stream.poll();
  REQUIRE(observation.status == neko::detail::stream_status::consumed);
  CHECK(observation.sequence == 3); // the newest offer wins
}

TEST_CASE("concurrent publishers serialize into one stream") {
  temporary_directory temporary;
  const auto root = temporary.path();

  constexpr int k_threads = 4;
  constexpr int k_publishes = 3;
  std::vector<std::vector<neko::detail::publish_result>> results(k_threads);
  std::vector<std::thread> threads;
  for (int thread = 0; thread < k_threads; ++thread) {
    threads.emplace_back([&results, root, thread] {
      for (int iteration = 0; iteration < k_publishes; ++iteration) {
        const auto tag = "t" + std::to_string(thread) + "-i" + std::to_string(iteration);
        results[static_cast<std::size_t>(thread)].push_back(publish(root, tag + "-a", tag + "-b"));
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }

  std::set<std::uint64_t> sequences;
  for (const auto& thread_results : results) {
    for (const auto& result : thread_results) {
      sequences.insert(result.sequence);
    }
  }
  REQUIRE(sequences.size() == k_threads * k_publishes);
  CHECK(*sequences.begin() == 1);
  CHECK(*sequences.rbegin() == k_threads * k_publishes);

  neko::detail::generation_stream stream{make_descriptor(), root};
  const auto observation = stream.poll();
  REQUIRE(observation.status == neko::detail::stream_status::consumed);
  CHECK(observation.sequence == k_threads * k_publishes);
}

#if !defined(_WIN32)

TEST_CASE("the stream lock serializes separate processes") {
  temporary_directory temporary;
  const auto root = temporary.path();

  constexpr int k_children = 2;
  constexpr int k_publishes = 3;
  for (int child = 0; child < k_children; ++child) {
    const auto pid = ::fork();
    if (pid == 0) {
      // The child only touches the filesystem and exits through _exit, so
      // doctest state never runs twice.
      for (int iteration = 0; iteration < k_publishes; ++iteration) {
        const auto tag = "p" + std::to_string(::getpid()) + "-i" + std::to_string(iteration);
        static_cast<void>(publish(root, tag + "-a", tag + "-b"));
      }
      ::_exit(0);
    }
    REQUIRE(pid > 0);
  }
  for (int child = 0; child < k_children; ++child) {
    int status = 0;
    REQUIRE(::wait(&status) > 0);
    REQUIRE(WIFEXITED(status));
    REQUIRE(WEXITSTATUS(status) == 0);
  }

  // Every offer file carries a distinct sequence 1..N and the consumer sees
  // the newest one.
  std::set<std::uint64_t> sequences;
  for (const auto& entry : std::filesystem::directory_iterator(root / "pub-e2e" / "offers")) {
    const auto name = entry.path().filename().string();
    if (name.ends_with(".ready")) {
      sequences.insert(neko::detail::parse_generation_offer_marker(name, name).sequence);
    }
  }
  REQUIRE(sequences.size() == k_children * k_publishes);
  CHECK(*sequences.rbegin() == k_children * k_publishes);

  neko::detail::generation_stream stream{make_descriptor(), root};
  const auto observation = stream.poll();
  REQUIRE(observation.status == neko::detail::stream_status::consumed);
  CHECK(observation.sequence == k_children * k_publishes);
}

#endif
