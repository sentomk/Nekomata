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

#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace {

constexpr std::uintptr_t kHint = 0x00400000; // near a typical -no-pie image

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
  void* arena = pages.reserve_code_near(kHint, 16);
  REQUIRE(arena != nullptr);

  std::vector<std::uint8_t> image(16, 0xC3); // ret sled
  CHECK(pages.commit_code(arena, image.data(), image.size()));

  const auto address = reinterpret_cast<std::uintptr_t>(arena);
  const std::int64_t delta = address > kHint ? static_cast<std::int64_t>(address - kHint)
                                             : static_cast<std::int64_t>(kHint - address);
  CHECK(delta < 0x7FFF'F000); // jmp rel32 reaches from the hint
}

TEST_CASE("committing past the reservation is rejected loudly") {
  neko::elf::code_pages pages;
  void* arena = pages.reserve_code_near(kHint, 16);
  REQUIRE(arena != nullptr);

  std::vector<std::uint8_t> oversized(16 + 4096, 0x00);
  CHECK_THROWS_AS(pages.commit_code(arena, oversized.data(), oversized.size()), std::runtime_error);
  try {
    (void)pages.commit_code(arena, oversized.data(), oversized.size());
  } catch (const std::runtime_error& e) {
    CHECK(std::strstr(e.what(), "exceeds its reservation") != nullptr);
  }
}

TEST_CASE("committing into an unknown pointer is rejected") {
  neko::elf::code_pages pages;
  int not_an_arena = 0;
  CHECK_THROWS_AS(pages.commit_code(&not_an_arena, &not_an_arena, 1), std::runtime_error);
}

TEST_CASE("the guard page after a reservation faults on touch") {
  neko::elf::code_pages pages;
  void* arena = pages.reserve_code_near(kHint, 16);
  REQUIRE(arena != nullptr);
  const std::uint64_t page = [] {
    const long v = sysconf(_SC_PAGESIZE);
    return v > 0 ? static_cast<std::uint64_t>(v) : std::uint64_t{4096};
  }();
  // First byte past the usable span = the guard page.
  volatile auto* guard = reinterpret_cast<volatile std::uint8_t*>(arena) + page;

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
