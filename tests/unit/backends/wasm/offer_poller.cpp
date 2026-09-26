#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <backends/wasm/offer_poller.hpp>

#include <protocol/wasm_offer.hpp>

#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using namespace neko::wasm;
using neko::detail::wasm_offer;

constexpr std::string_view a_digest =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr std::string_view b_digest =
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

class test_loader final : public module_loader {
public:
  void open(std::string path, std::string_view sha256, completion complete) override {
    opened_paths.emplace_back(std::move(path));
    opened_digests.emplace_back(sha256);
    pending = std::move(complete);
  }
  void finish(std::unique_ptr<module_image> image, std::string message = {}) {
    auto callback = std::move(pending);
    callback({std::move(image), std::move(message)});
  }
  std::vector<std::string> opened_paths;
  std::vector<std::string> opened_digests;
  completion pending;
};

class scripted_fetcher final : public manifest_fetcher {
public:
  void fetch(std::string url, completion complete) override {
    fetched_urls.emplace_back(std::move(url));
    pending = std::move(complete);
    ++fetches;
  }
  void deliver_ok(std::string text) {
    auto callback = std::move(pending);
    callback({true, std::move(text), {}});
  }
  void deliver_failure(std::string message = {}) {
    auto callback = std::move(pending);
    callback({false, {}, std::move(message)});
  }
  std::vector<std::string> fetched_urls;
  completion pending;
  int fetches = 0;
};

class test_image final : public module_image {
public:
  module_entry entries[1];
  module_descriptor value{
      {module_interface_version, sizeof(module_descriptor)}, "test-v1", 1, entries};
  const module_header* descriptor() const noexcept override { return &value.header; }
  void keep_resident() noexcept override {}
};

std::string offer_text(std::uint64_t sequence, std::string_view generation, std::string_view digest,
                       std::string_view artifact = "modules/gen.wasm") {
  return "nekomata-wasm/1\n"
         "group_id \"game\"\n"
         "sequence " +
         std::to_string(sequence) +
         "\n"
         "generation_id \"" +
         std::string(generation) +
         "\"\n"
         "abi_id \"test-v1\"\n"
         "artifact \"" +
         std::string(artifact) + "\" \"" + std::string(digest) + "\"\n" + "entry \"tick\"\n";
}

wasm_offer offer_value(std::uint64_t sequence, std::string_view generation,
                       std::string_view digest) {
  wasm_offer value;
  value.group_id = "game";
  value.sequence = sequence;
  value.generation_id = generation;
  value.abi_id = "test-v1";
  value.artifact_path = "modules/gen.wasm";
  value.sha256 = digest;
  value.entries = {"tick"};
  return value;
}

struct event_log {
  std::vector<offer_event_kind> kinds;
  std::vector<std::string> messages;

  void record(const offer_event& event) {
    kinds.push_back(event.kind);
    messages.push_back(event.message);
  }
};

} // namespace

TEST_CASE("artifact URLs join the manifest directory lexically") {
  CHECK(resolve_artifact_url("offers/latest", "modules/gen-1.wasm") == "offers/modules/gen-1.wasm");
  CHECK(resolve_artifact_url("http://host/offers/latest", "modules/gen-1.wasm") ==
        "http://host/offers/modules/gen-1.wasm");
  CHECK(resolve_artifact_url("latest", "modules/gen-1.wasm") == "modules/gen-1.wasm");
}

TEST_CASE("an empty manifest URL is a configuration error") {
  test_loader loader;
  scripted_fetcher fetcher;
  REQUIRE_THROWS_AS((offer_poller{loader, fetcher, ""}), std::runtime_error);
}

TEST_CASE("a superseding offer becomes a loading candidate") {
  test_loader loader;
  scripted_fetcher fetcher;
  event_log log;
  offer_poller poller{loader, fetcher, "offers/latest",
                      [&log](const offer_event& event) { log.record(event); }};
  REQUIRE(poller.pending() == nullptr);
  REQUIRE(poller.accepted() == nullptr);

  poller.poll();
  REQUIRE(fetcher.fetches == 1);
  REQUIRE(fetcher.fetched_urls.back() == "offers/latest");
  fetcher.deliver_ok(offer_text(7, "gen-7", a_digest));

  REQUIRE(log.kinds.size() == 1);
  CHECK(log.kinds.back() == offer_event_kind::offer_accepted);
  CHECK(log.messages.back() == "generation 'gen-7' at sequence 7");
  REQUIRE(poller.pending() != nullptr);
  CHECK(poller.pending()->status() == candidate_status::loading);
  REQUIRE(poller.accepted() != nullptr);
  CHECK(*poller.accepted() == offer_value(7, "gen-7", a_digest));

  // The artifact URL is resolved against the manifest directory and the
  // contract carries the offer's digest.
  REQUIRE(loader.opened_paths.size() == 1);
  CHECK(loader.opened_paths.back() == "offers/modules/gen.wasm");
  CHECK(loader.opened_digests.back() == a_digest);
}

TEST_CASE("polling is suppressed while a fetch is outstanding") {
  test_loader loader;
  scripted_fetcher fetcher;
  offer_poller poller{loader, fetcher, "offers/latest"};
  poller.poll();
  poller.poll();
  poller.poll();
  CHECK(fetcher.fetches == 1);
  fetcher.deliver_ok(offer_text(1, "gen-1", a_digest));
  poller.poll();
  CHECK(fetcher.fetches == 2);
}

TEST_CASE("stale, duplicate, and conflicting offers never touch the loader") {
  test_loader loader;
  scripted_fetcher fetcher;
  event_log log;
  offer_poller poller{loader, fetcher, "offers/latest",
                      [&log](const offer_event& event) { log.record(event); }};

  poller.poll();
  fetcher.deliver_ok(offer_text(7, "gen-7", a_digest));
  REQUIRE(loader.opened_paths.size() == 1);

  SUBCASE("duplicate") {
    poller.poll();
    fetcher.deliver_ok(offer_text(7, "gen-7", a_digest));
    CHECK(log.kinds.back() == offer_event_kind::offer_ignored);
    CHECK(log.messages.back() == "duplicate generation 'gen-7'");
  }
  SUBCASE("stale") {
    poller.poll();
    fetcher.deliver_ok(offer_text(6, "gen-6", a_digest));
    CHECK(log.kinds.back() == offer_event_kind::offer_ignored);
    CHECK(log.messages.back() == "stale sequence 6 behind 7");
  }
  SUBCASE("conflicting identity") {
    poller.poll();
    fetcher.deliver_ok(offer_text(7, "gen-7-rival", a_digest));
    CHECK(log.kinds.back() == offer_event_kind::offer_conflict);
    CHECK(log.messages.back() == "sequence 7 claimed by generations 'gen-7' and 'gen-7-rival'");
  }
  SUBCASE("conflicting digest under the same identity") {
    poller.poll();
    fetcher.deliver_ok(offer_text(7, "gen-7", b_digest));
    CHECK(log.kinds.back() == offer_event_kind::offer_conflict);
  }
  CHECK(loader.opened_paths.size() == 1);
  REQUIRE(poller.accepted() != nullptr);
  CHECK(poller.accepted()->generation_id == "gen-7");
}

TEST_CASE("manifest rejections and fetch failures report and keep polling") {
  test_loader loader;
  scripted_fetcher fetcher;
  event_log log;
  offer_poller poller{loader, fetcher, "offers/latest",
                      [&log](const offer_event& event) { log.record(event); }};

  poller.poll();
  fetcher.deliver_ok("not-an-offer\n");
  CHECK(log.kinds.back() == offer_event_kind::manifest_invalid);
  CHECK(log.messages.back().find("expected nekomata-wasm/1") != std::string::npos);

  poller.poll();
  fetcher.deliver_failure("http 404");
  CHECK(log.kinds.back() == offer_event_kind::manifest_fetch_failed);
  CHECK(log.messages.back() == "http 404");

  poller.poll();
  fetcher.deliver_ok(offer_text(7, "gen-7", a_digest));
  CHECK(log.kinds.back() == offer_event_kind::offer_accepted);
  REQUIRE(poller.accepted() != nullptr);
  CHECK(poller.accepted()->sequence == 7);
}

TEST_CASE("a superseding offer replaces the pending candidate") {
  test_loader loader;
  scripted_fetcher fetcher;
  event_log log;
  offer_poller poller{loader, fetcher, "offers/latest",
                      [&log](const offer_event& event) { log.record(event); }};

  poller.poll();
  fetcher.deliver_ok(offer_text(7, "gen-7", a_digest));
  REQUIRE(loader.opened_paths.size() == 1);
  const candidate* first = poller.pending();
  REQUIRE(first != nullptr);

  poller.poll();
  fetcher.deliver_ok(offer_text(8, "gen-8", b_digest));
  CHECK(log.kinds.back() == offer_event_kind::offer_accepted);
  CHECK(loader.opened_paths.size() == 2);
  CHECK(loader.opened_digests.back() == b_digest);
  // The superseded candidate was destroyed with its poller slot; only the
  // newest candidate remains addressable.
  REQUIRE(poller.pending() != nullptr);
  CHECK(poller.pending() != first);
  REQUIRE(poller.accepted() != nullptr);
  CHECK(poller.accepted()->generation_id == "gen-8");
}

TEST_CASE("a completed candidate stays observable through the poller") {
  test_loader loader;
  scripted_fetcher fetcher;
  offer_poller poller{loader, fetcher, "offers/latest"};
  poller.poll();
  fetcher.deliver_ok(offer_text(7, "gen-7", a_digest));

  auto image = std::make_unique<test_image>();
  image->entries[0] = {"tick", []() {}};
  loader.finish(std::move(image));
  CHECK(poller.pending()->status() == candidate_status::ready);
  CHECK(poller.accepted()->sequence == 7);
}

TEST_CASE("destruction discards a late manifest delivery") {
  test_loader loader;
  scripted_fetcher fetcher;
  {
    offer_poller poller{loader, fetcher, "offers/latest"};
    poller.poll();
  }
  fetcher.deliver_ok(offer_text(7, "gen-7", a_digest));
  CHECK(loader.opened_paths.empty());
}

TEST_CASE("repeated diagnostics reach the sink once per change") {
  std::vector<std::string> seen;
  const auto report = suppress_repeats([&](const offer_event& event) {
    seen.push_back(std::to_string(static_cast<int>(event.kind)) + ":" + event.message);
  });
  const offer_event accepted{offer_event_kind::offer_accepted, "generation 'g' at sequence 1"};
  const offer_event duplicate{offer_event_kind::offer_ignored, "duplicate generation 'g'"};
  const offer_event failed{offer_event_kind::manifest_fetch_failed, "manifest fetch failed"};

  report(accepted);
  for (int poll = 0; poll < 50; ++poll) {
    report(duplicate);
  }
  report(failed);
  report(failed);
  report(duplicate);
  // Same message under a different kind is a change.
  report({offer_event_kind::offer_conflict, "duplicate generation 'g'"});
  CHECK(seen == std::vector<std::string>{"0:generation 'g' at sequence 1",
                                         "1:duplicate generation 'g'", "4:manifest fetch failed",
                                         "1:duplicate generation 'g'",
                                         "2:duplicate generation 'g'"});

  // Each wrapper keeps its own history.
  std::vector<std::string> other;
  const auto second =
      suppress_repeats([&](const offer_event& event) { other.push_back(event.message); });
  second(duplicate);
  CHECK(other == std::vector<std::string>{"duplicate generation 'g'"});
}
