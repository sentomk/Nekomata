// Guard-page and span-verification tests for the ELF code pages.
//
// These encode the runtime invariants from CONTRIBUTING.md's testing
// contract: writes past a reservation must be loud, never silent. The
// guard page is verified by letting a forked child touch it and checking
// it dies of SIGSEGV; the span check is verified by asking commit_code
// for more bytes than were reserved and expecting a thrown rejection.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "code_pages.hpp"

#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace {

constexpr std::uintptr_t hint_address = 0x00400000; // near a typical -no-pie image

std::size_t system_page_size() {
  const long value = sysconf(_SC_PAGESIZE);
  return value > 0 ? static_cast<std::size_t>(value) : std::size_t{4096};
}

// Under ASan the runtime intercepts SIGSEGV by default and turns it into
// its own report + exit(1) — the forked child must die of the *signal*
// for WIFSIGNALED to see the guard page firing, so disable the
// interception process-wide (weak definition, only linked under ASan).
#if defined(__clang__) && defined(__has_feature)
#if __has_feature(address_sanitizer)
extern "C" __attribute__((used)) const char* __asan_default_options() {
  return "handle_segv=0";
}
#endif
#endif

} // namespace

TEST_CASE("reservations land within rel32 reach and can be committed") {
  neko::elf::code_pages pages;
  auto allocation = pages.reserve_code_near(hint_address, 16);
  REQUIRE(allocation != nullptr);

  std::vector<std::uint8_t> image(16, 0xC3); // ret sled
  CHECK(pages.commit_code(*allocation, image.data(), image.size()));

  const auto address = reinterpret_cast<std::uintptr_t>(allocation->data());
  const std::int64_t delta = address > hint_address
                                 ? static_cast<std::int64_t>(address - hint_address)
                                 : static_cast<std::int64_t>(hint_address - address);
  CHECK(delta < 0x7FFF'F000); // jmp rel32 reaches from the hint
}

TEST_CASE("committing past the reservation is rejected loudly") {
  neko::elf::code_pages pages;
  auto allocation = pages.reserve_code_near(hint_address, 16);
  REQUIRE(allocation != nullptr);

  std::vector<std::uint8_t> oversized(16 + 4096, 0x00);
  CHECK_THROWS_AS(pages.commit_code(*allocation, oversized.data(), oversized.size()),
                  std::runtime_error);
  try {
    (void)pages.commit_code(*allocation, oversized.data(), oversized.size());
  } catch (const std::runtime_error& e) {
    CHECK(std::strstr(e.what(), "exceeds its reservation") != nullptr);
  }
}

TEST_CASE("committing into an unknown pointer is rejected") {
  neko::elf::code_pages pages;
  neko::elf::code_pages other_pages;
  auto foreign = other_pages.reserve_code_near(hint_address, 1);
  REQUIRE(foreign != nullptr);
  std::uint8_t byte = 0;
  CHECK_THROWS_AS(pages.commit_code(*foreign, &byte, 1), std::runtime_error);
}

TEST_CASE("the guard page after a reservation faults on touch") {
  neko::elf::code_pages pages;
  auto allocation = pages.reserve_code_near(hint_address, 16);
  REQUIRE(allocation != nullptr);
  const auto page = system_page_size();
  // First byte past the usable span = the guard page.
  volatile auto* guard = reinterpret_cast<volatile std::uint8_t*>(allocation->data()) + page;

  const pid_t child = fork();
  REQUIRE(child >= 0);
  if (child == 0) {
    *guard = 1; // must die of SIGSEGV before reaching the exit
    _exit(0);   // if we get here the guard is not armed
  }
  int status = 0;
  REQUIRE(waitpid(child, &status, 0) == child);
  CHECK(WIFSIGNALED(status));
  CHECK(WTERMSIG(status) == SIGSEGV);
}

TEST_CASE("destroying a candidate allocation returns its slot to the pool") {
  neko::elf::code_pages pages;
  void* address = nullptr;
  {
    auto allocation = pages.reserve_code_near(hint_address, 16);
    REQUIRE(allocation != nullptr);
    address = allocation->data();
    unsigned char residency = 0;
    REQUIRE(mincore(address, system_page_size(), &residency) == 0);
  }

  unsigned char residency = 0;
  CHECK(mincore(address, system_page_size(), &residency) == 0);
  auto reused = pages.reserve_code_near(hint_address, 16);
  REQUIRE(reused != nullptr);
  CHECK(reused->data() == address);
}

TEST_CASE("writable candidate storage remains mutable and returns its slot") {
  neko::elf::code_pages pages;
  void* address = nullptr;
  {
    auto allocation = pages.reserve_writable_near(hint_address, sizeof(std::uint64_t));
    REQUIRE(allocation != nullptr);
    address = allocation->data();
    auto* value = static_cast<std::uint64_t*>(allocation->data());
    *value = 0x0123'4567'89AB'CDEF;
    CHECK(*value == 0x0123'4567'89AB'CDEF);
  }

  unsigned char residency = 0;
  CHECK(mincore(address, system_page_size(), &residency) == 0);
  auto reused = pages.reserve_writable_near(hint_address, sizeof(std::uint64_t));
  REQUIRE(reused != nullptr);
  CHECK(reused->data() == address);
  *static_cast<std::uint64_t*>(reused->data()) = 42;
  CHECK(*static_cast<const std::uint64_t*>(reused->data()) == 42);
}

TEST_CASE("one near-code pool serves hundreds of live allocations") {
  constexpr std::size_t allocation_count = 512;
  neko::elf::code_pages pages;
  std::vector<neko::backend::executable_allocation_ptr> allocations;
  allocations.reserve(allocation_count);
  std::vector<std::uintptr_t> addresses;
  addresses.reserve(allocation_count);

  for (std::size_t index = 0; index < allocation_count; ++index) {
    auto allocation = pages.reserve_code_near(hint_address, 16);
    REQUIRE(allocation != nullptr);
    const auto address = reinterpret_cast<std::uintptr_t>(allocation->data());
    const auto delta = address > hint_address ? address - hint_address : hint_address - address;
    CHECK(delta < 0x7FFF'F000);
    addresses.push_back(address);
    allocations.push_back(std::move(allocation));
  }

  std::sort(addresses.begin(), addresses.end());
  CHECK(std::adjacent_find(addresses.begin(), addresses.end()) == addresses.end());
  CHECK(addresses.back() - addresses.front() < 64ull * 1024 * 1024);
}

TEST_CASE("a hint outside an existing pool gets its own reachable pool") {
  const auto far_hint = reinterpret_cast<std::uintptr_t>(&system_page_size);
  neko::elf::code_pages pages;
  auto near = pages.reserve_code_near(hint_address, 16);
  auto far = pages.reserve_code_near(far_hint, 16);
  REQUIRE(near != nullptr);
  REQUIRE(far != nullptr);

  const auto near_address = reinterpret_cast<std::uintptr_t>(near->data());
  const auto far_address = reinterpret_cast<std::uintptr_t>(far->data());
  const auto near_delta =
      near_address > hint_address ? near_address - hint_address : hint_address - near_address;
  const auto far_delta = far_address > far_hint ? far_address - far_hint : far_hint - far_address;
  const auto pool_delta =
      far_address > near_address ? far_address - near_address : near_address - far_address;
  CHECK(near_delta < 0x7FFF'F000);
  CHECK(far_delta < 0x7FFF'F000);
  CHECK(pool_delta > 0x7FFF'F000);
}

TEST_CASE("process-lifetime allocations survive their handle and substituter") {
  void* address = nullptr;
  {
    neko::elf::code_pages pages;
    auto allocation = pages.reserve_code_near(hint_address, 16);
    REQUIRE(allocation != nullptr);
    address = allocation->data();
    allocation->release_to_process();
  }

  unsigned char residency = 0;
  REQUIRE(mincore(address, system_page_size(), &residency) == 0);
  CHECK(munmap(address, system_page_size() * 2) == 0);
}
