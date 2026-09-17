// Allocation and entry-patch policy tests for the PE code pages.
//
// These encode the runtime invariants from CONTRIBUTING.md's testing
// contract: writes past a reservation must be loud, never silent. The
// uncommitted guard page is verified by touching it under SEH and observing
// the access violation; the span check is verified by asking commit_code for
// more bytes than were reserved and expecting a thrown rejection. The
// prologue cases encode the surveyed MSVC /Od entry shapes.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "code_pages.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace {

// A live code address typical of the module hosting a reloadable function.
std::uintptr_t module_hint() {
  return reinterpret_cast<std::uintptr_t>(&module_hint);
}

// SEH guard: no C++ objects in a function using __try (C2712).
int touch_faults(volatile std::uint8_t* at) {
  __try {
    *at = 1;
    return 0; // the guard is not armed if we reach this
  } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER
                                                               : EXCEPTION_CONTINUE_SEARCH) {
    return 1;
  }
}

std::size_t system_page_size() {
  SYSTEM_INFO info{};
  GetSystemInfo(&info);
  return info.dwPageSize > 0 ? info.dwPageSize : 4096;
}

} // namespace

TEST_CASE("reservations land within rel32 reach and committed code runs") {
  neko::pe::code_pages pages;
  auto allocation = pages.reserve_code_near(module_hint(), 16);
  REQUIRE(allocation != nullptr);

  // mov eax, 42; ret
  const std::uint8_t image[] = {0xB8, 0x2A, 0x00, 0x00, 0x00, 0xC3};
  REQUIRE(pages.commit_code(*allocation, image, sizeof(image)));

  const auto address = reinterpret_cast<std::uintptr_t>(allocation->data());
  const std::int64_t delta = address > module_hint()
                                 ? static_cast<std::int64_t>(address - module_hint())
                                 : static_cast<std::int64_t>(module_hint() - address);
  CHECK(delta < 0x7FFF'F000); // jmp rel32 reaches from the hint
  CHECK(reinterpret_cast<int (*)()>(allocation->data())() == 42);
}

TEST_CASE("committing past the reservation is rejected loudly") {
  neko::pe::code_pages pages;
  auto allocation = pages.reserve_code_near(module_hint(), 16);
  REQUIRE(allocation != nullptr);

  std::vector<std::uint8_t> oversized(16 + system_page_size(), 0x00);
  CHECK_THROWS_AS(pages.commit_code(*allocation, oversized.data(), oversized.size()),
                  std::runtime_error);
  try {
    (void)pages.commit_code(*allocation, oversized.data(), oversized.size());
  } catch (const std::runtime_error& e) {
    CHECK(std::strstr(e.what(), "exceeds its reservation") != nullptr);
  }
}

TEST_CASE("committing into an unknown pointer is rejected") {
  neko::pe::code_pages pages;
  neko::pe::code_pages other_pages;
  auto foreign = other_pages.reserve_code_near(module_hint(), 1);
  REQUIRE(foreign != nullptr);
  std::uint8_t byte = 0;
  CHECK_THROWS_AS(pages.commit_code(*foreign, &byte, 1), std::runtime_error);
}

TEST_CASE("the uncommitted page after a reservation faults on touch") {
  neko::pe::code_pages pages;
  auto allocation = pages.reserve_code_near(module_hint(), 16);
  REQUIRE(allocation != nullptr);
  // First byte past the usable span is still inside our reservation, but
  // deliberately left uncommitted.
  auto* guard = static_cast<std::uint8_t*>(allocation->data()) + system_page_size();
  CHECK(touch_faults(guard) == 1);
}

TEST_CASE("destroying a candidate allocation returns its slot to the pool") {
  neko::pe::code_pages pages;
  void* address = nullptr;
  {
    auto allocation = pages.reserve_code_near(module_hint(), 16);
    REQUIRE(allocation != nullptr);
    address = allocation->data();
  }

  auto reused = pages.reserve_code_near(module_hint(), 16);
  REQUIRE(reused != nullptr);
  CHECK(reused->data() == address);
}

TEST_CASE("writable candidate storage remains mutable and returns its slot") {
  neko::pe::code_pages pages;
  void* address = nullptr;
  {
    auto allocation = pages.reserve_writable_near(module_hint(), sizeof(std::uint64_t));
    REQUIRE(allocation != nullptr);
    address = allocation->data();
    auto* value = static_cast<std::uint64_t*>(allocation->data());
    *value = 0x0123'4567'89AB'CDEF;
    CHECK(*value == 0x0123'4567'89AB'CDEF);
  }

  auto reused = pages.reserve_writable_near(module_hint(), sizeof(std::uint64_t));
  REQUIRE(reused != nullptr);
  CHECK(reused->data() == address);
  *static_cast<std::uint64_t*>(reused->data()) = 42;
  CHECK(*static_cast<const std::uint64_t*>(reused->data()) == 42);
}

TEST_CASE("one near-code pool serves hundreds of live allocations") {
  constexpr std::size_t allocation_count = 512;
  neko::pe::code_pages pages;
  std::vector<neko::backend::executable_allocation_ptr> allocations;
  allocations.reserve(allocation_count);
  std::vector<std::uintptr_t> addresses;
  addresses.reserve(allocation_count);

  for (std::size_t index = 0; index < allocation_count; ++index) {
    auto allocation = pages.reserve_code_near(module_hint(), 16);
    REQUIRE(allocation != nullptr);
    const auto address = reinterpret_cast<std::uintptr_t>(allocation->data());
    const auto delta = address > module_hint() ? address - module_hint() : module_hint() - address;
    CHECK(delta < 0x7FFF'F000);
    addresses.push_back(address);
    allocations.push_back(std::move(allocation));
  }

  std::sort(addresses.begin(), addresses.end());
  CHECK(std::adjacent_find(addresses.begin(), addresses.end()) == addresses.end());
  CHECK(addresses.back() - addresses.front() < 64ull * 1024 * 1024);
}

TEST_CASE("a hint outside an existing pool gets its own reachable pool") {
  neko::pe::code_pages pages;
  auto near_allocation = pages.reserve_code_near(module_hint(), 16);
  REQUIRE(near_allocation != nullptr);
  const auto near_address = reinterpret_cast<std::uintptr_t>(near_allocation->data());

  // Three GiB above the existing pool: pools center on their hint, so a
  // 2 GiB offset would still overlap rel32 range by half a pool. Three GiB
  // guarantees the second pool starts beyond rel32 of the near pool.
  const auto far_hint = near_address + 0xC000'0000;
  auto far_allocation = pages.reserve_code_near(far_hint, 16);
  REQUIRE(far_allocation != nullptr);
  const auto far_address = reinterpret_cast<std::uintptr_t>(far_allocation->data());

  const auto near_delta =
      near_address > module_hint() ? near_address - module_hint() : module_hint() - near_address;
  const auto far_delta = far_address > far_hint ? far_address - far_hint : far_hint - far_address;
  const auto pool_delta =
      far_address > near_address ? far_address - near_address : near_address - far_address;
  CHECK(near_delta < 0x7FFF'F000);
  CHECK(far_delta < 0x7FFF'F000);
  CHECK(pool_delta > 0x7FFF'F000);
}

TEST_CASE("process-lifetime allocations survive their handle and substituter") {
  void* address = nullptr;
  std::uint8_t first_byte = 0;
  {
    neko::pe::code_pages pages;
    auto allocation = pages.reserve_code_near(module_hint(), 16);
    REQUIRE(allocation != nullptr);
    address = allocation->data();
    const std::uint8_t image[] = {0xC3}; // ret
    REQUIRE(pages.commit_code(*allocation, image, sizeof(image)));
    first_byte = *static_cast<const std::uint8_t*>(address);
    allocation->release_to_process();
  }

  MEMORY_BASIC_INFORMATION info{};
  REQUIRE(VirtualQuery(address, &info, sizeof(info)) == sizeof(info));
  CHECK(info.State == MEM_COMMIT);
  CHECK(info.Protect == PAGE_EXECUTE_READ);
  CHECK(*static_cast<const std::uint8_t*>(address) == first_byte);

  // Best-effort cleanup: the slot is the first in its 64 KiB-aligned pool.
  SYSTEM_INFO system{};
  GetSystemInfo(&system);
  const auto granularity = static_cast<std::uintptr_t>(system.dwAllocationGranularity);
  VirtualFree(
      reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(address) & ~(granularity - 1)), 0,
      MEM_RELEASE);
}

TEST_CASE("precheck accepts the surveyed MSVC /Od prologue families") {
  neko::pe::code_pages pages;
  auto target = pages.reserve_code_near(module_hint(), 16);
  REQUIRE(target != nullptr);

  // Argument spills: ecx, r8d, rcx, r9 in their home slots.
  const std::uint8_t spills[][8] = {
      {0x89, 0x4C, 0x24, 0x08},       // mov [rsp+08h], ecx
      {0x44, 0x89, 0x44, 0x24, 0x18}, // mov [rsp+18h], r8d
      {0x48, 0x89, 0x4C, 0x24, 0x08}, // mov [rsp+08h], rcx
      {0x4C, 0x89, 0x4C, 0x24, 0x20}, // mov [rsp+20h], r9
      {0x89, 0x54, 0x24, 0x10},       // mov [rsp+10h], edx
      {0x48, 0x89, 0x54, 0x24, 0x10}, // mov [rsp+10h], rdx
  };
  for (const auto& prologue : spills) {
    auto fixture = pages.reserve_writable_near(module_hint(), sizeof(prologue));
    REQUIRE(fixture != nullptr);
    std::memcpy(fixture->data(), prologue, sizeof(prologue));
    CHECK(pages.precheck_entry(reinterpret_cast<std::uintptr_t>(fixture->data()), target->data()));
  }

  // Frame allocation for functions without register arguments.
  const std::uint8_t frames[][8] = {
      {0x48, 0x83, 0xEC, 0x28},                   // sub rsp, 28h
      {0x48, 0x81, 0xEC, 0x38, 0x02, 0x00, 0x00}, // sub rsp, 238h
      // clang-cl with frame pointers explicitly enabled.
      {0x55, 0x48, 0x89, 0xE5, 0x48}, // push rbp; mov rbp,rsp; (sub rsp ...)
      {0xF3, 0x0F, 0x1E, 0xFA, 0x55}, // endbr64; push rbp
  };
  for (const auto& prologue : frames) {
    auto fixture = pages.reserve_writable_near(module_hint(), sizeof(prologue));
    REQUIRE(fixture != nullptr);
    std::memcpy(fixture->data(), prologue, sizeof(prologue));
    CHECK(pages.precheck_entry(reinterpret_cast<std::uintptr_t>(fixture->data()), target->data()));
  }
}

TEST_CASE("precheck refuses unknown prologues, foreign spills, and thunks") {
  neko::pe::code_pages pages;
  auto target = pages.reserve_code_near(module_hint(), 16);
  REQUIRE(target != nullptr);

  struct case_t {
    std::vector<std::uint8_t> bytes;
  };
  const std::vector<case_t> refused = {
      // Callee-saved spill: rbx is not one of the four register arguments.
      {{0x48, 0x89, 0x5C, 0x24, 0x08}},
      // An indirect tail jump.
      {{0xFF, 0x25, 0x00, 0x00, 0x00}},
      // Arbitrary junk.
      {{0x12, 0x34, 0x56, 0x78, 0x9A}},
  };
  for (const auto& entry_case : refused) {
    auto fixture = pages.reserve_writable_near(module_hint(), entry_case.bytes.size());
    REQUIRE(fixture != nullptr);
    std::memcpy(fixture->data(), entry_case.bytes.data(), entry_case.bytes.size());
    CHECK_FALSE(
        pages.precheck_entry(reinterpret_cast<std::uintptr_t>(fixture->data()), target->data()));
  }

  // An incremental-link thunk: E9 whose target is module code, not one of
  // our committed slots.
  auto thunk = pages.reserve_writable_near(module_hint(), 8);
  REQUIRE(thunk != nullptr);
  auto* bytes = static_cast<std::uint8_t*>(thunk->data());
  bytes[0] = 0xE9;
  const std::int64_t rel = static_cast<std::int64_t>(module_hint()) -
                           static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(bytes) + 5);
  CHECK(rel >= -0x8000'0000LL);
  CHECK(rel <= 0x7FFF'FFFF);
  std::int32_t narrow = static_cast<std::int32_t>(rel);
  std::memcpy(bytes + 1, &narrow, sizeof(narrow));
  CHECK_FALSE(pages.precheck_entry(reinterpret_cast<std::uintptr_t>(bytes), target->data()));
}

TEST_CASE("patch_entry writes a reachable redirect and restore round-trips") {
  neko::pe::code_pages pages;
  auto first = pages.reserve_code_near(module_hint(), 16);
  auto second = pages.reserve_code_near(module_hint(), 16);
  REQUIRE(first != nullptr);
  REQUIRE(second != nullptr);
  // Redirect targets must hold committed code; only commit_code marks a
  // slot as a valid E9 landing zone.
  const std::uint8_t sled[] = {0xC3}; // ret
  REQUIRE(pages.commit_code(*first, sled, sizeof(sled)));
  REQUIRE(pages.commit_code(*second, sled, sizeof(sled)));

  auto fixture = pages.reserve_writable_near(module_hint(), 16);
  REQUIRE(fixture != nullptr);
  const std::uint8_t original[] = {0x48, 0x89, 0x4C, 0x24, 0x08, 0x48, 0x83, 0xEC};
  std::memcpy(fixture->data(), original, sizeof(original));
  const auto entry = reinterpret_cast<std::uintptr_t>(fixture->data());

  std::uint8_t snapshot[5] = {};
  REQUIRE(pages.snapshot_entry(entry, snapshot));
  CHECK(std::memcmp(snapshot, original, 5) == 0);

  MEMORY_BASIC_INFORMATION before{};
  REQUIRE(VirtualQuery(fixture->data(), &before, sizeof(before)) == sizeof(before));

  REQUIRE(pages.patch_entry(entry, first->data()));
  const auto* patched = reinterpret_cast<const std::uint8_t*>(entry);
  REQUIRE(patched[0] == 0xE9);
  std::int32_t written = 0;
  std::memcpy(&written, patched + 1, sizeof(written));
  CHECK(static_cast<std::uintptr_t>(static_cast<std::int64_t>(entry) + 5 + written) ==
        reinterpret_cast<std::uintptr_t>(first->data()));

  // Re-patching a previous redirect stays allowed: repeated reloads work.
  CHECK(pages.precheck_entry(entry, second->data()));
  REQUIRE(pages.patch_entry(entry, second->data()));

  REQUIRE(pages.restore_entry(entry, snapshot));
  CHECK(std::memcmp(reinterpret_cast<const void*>(entry), original, 5) == 0);

  MEMORY_BASIC_INFORMATION after{};
  REQUIRE(VirtualQuery(fixture->data(), &after, sizeof(after)) == sizeof(after));
  CHECK(after.Protect == before.Protect);
}
