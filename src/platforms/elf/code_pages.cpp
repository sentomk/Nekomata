#include "code_pages.hpp"

#include <neko/log.hpp>

#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

namespace neko::elf {
namespace {

std::uint64_t page_size() {
  static const std::uint64_t size = [] {
    const long v = sysconf(_SC_PAGESIZE);
    return v > 0 ? static_cast<std::uint64_t>(v) : std::uint64_t{4096};
  }();
  return size;
}

std::uint64_t round_up(std::uint64_t v, std::uint64_t align) {
  return (v + align - 1) / align * align;
}

/// ±2 GiB minus slack, so `jmp rel32` from anywhere in the old function
/// reaches anywhere in the new image.
constexpr std::int64_t kRel32Limit = 0x7FFF'F000;

bool within_rel32(std::uintptr_t a, std::uintptr_t b) {
  const std::int64_t delta = static_cast<std::int64_t>(a > b ? a - b : b - a);
  return delta < kRel32Limit;
}

} // namespace

code_pages::~code_pages() = default;

void* code_pages::reserve_code_near(std::uintptr_t hint, std::uint64_t bytes) {
  if (bytes == 0) {
    return nullptr;
  }
  const std::uint64_t page = page_size();
  const std::uint64_t span = round_up(bytes, page);
  const std::uint64_t total = span + page; // usable span + PROT_NONE guard page
  for (int step = 1; step <= 16; ++step) {
    for (const std::int64_t sign : {std::int64_t{1}, std::int64_t{-1}}) {
      const std::uintptr_t addr = hint + sign * static_cast<std::uintptr_t>(step) * 0x0800'0000ull;
      void* mapping = mmap(reinterpret_cast<void*>(addr), total, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
      if (mapping == MAP_FAILED) {
        continue;
      }
      const auto begin = reinterpret_cast<std::uintptr_t>(mapping);
      if (!within_rel32(begin, hint)) {
        munmap(mapping, total); // kernel placed it too far away
        continue;
      }
      if (mprotect(reinterpret_cast<void*>(begin + span), page, PROT_NONE) != 0) {
        munmap(mapping, total); // cannot arm the guard — try the next hint
        continue;
      }
      arenas_.push_back({begin, begin + span});
      return mapping;
    }
  }
  return nullptr;
}

bool code_pages::owns_address(std::uintptr_t address) const {
  for (const auto& arena : arenas_) {
    if (address >= arena.begin && address < arena.end) {
      return true;
    }
  }
  return false;
}

bool code_pages::commit_code(void* reservation, const void* image, std::uint64_t bytes) {
  if (reservation == nullptr || image == nullptr || bytes == 0) {
    return false;
  }
  // Ownership + span invariant: the image must fit the reservation exactly
  // as it was sized. A larger image means the layout grew after the
  // reservation — reject loudly instead of writing past the span (the
  // guard page would catch it as a crash; this turns it into a message).
  const auto begin = reinterpret_cast<std::uintptr_t>(reservation);
  const auto* arena = [&]() -> const arena_range* {
    for (const auto& a : arenas_) {
      if (a.begin == begin) {
        return &a;
      }
    }
    return nullptr;
  }();
  if (arena == nullptr) {
    throw std::runtime_error("commit_code: not a reservation made by this substituter");
  }
  if (bytes > arena->end - arena->begin) {
    throw std::runtime_error(
        "code image (" + std::to_string(bytes) + " bytes) exceeds its reservation (" +
        std::to_string(arena->end - arena->begin) + " bytes) — refusing to commit");
  }
  std::memcpy(reservation, image, bytes);
  if (mprotect(reservation, arena->end - arena->begin, PROT_READ | PROT_EXEC) != 0) {
    throw std::runtime_error(std::string("mprotect(PROT_EXEC) failed: ") + std::strerror(errno));
  }
  __builtin___clear_cache(static_cast<char*>(reservation), static_cast<char*>(reservation) + bytes);
  return true;
}

bool code_pages::patchable_entry(std::uintptr_t entry, void* target) const {
  const auto* code = reinterpret_cast<const std::uint8_t*>(entry);
  const bool prologue_push_rbp =
      code[0] == 0x55 && code[1] == 0x48 && code[2] == 0x89 && code[3] == 0xE5;
  const bool prologue_endbr64 =
      code[0] == 0xF3 && code[1] == 0x0F && code[2] == 0x1E && code[3] == 0xFA && code[4] == 0x55;
  bool patchable = prologue_push_rbp || prologue_endbr64;
  if (!patchable && code[0] == 0xE9) {
    // Possibly one of OUR previous redirects: an E9 whose target lands
    // inside an arena we allocated can only have been written by us (a
    // genuine -O0 prologue never starts with a jmp). Re-patching in
    // place is what makes repeated reloads of the same function work.
    std::int32_t previous_rel = 0;
    std::memcpy(&previous_rel, code + 1, sizeof(previous_rel));
    patchable = owns_address(
        static_cast<std::uintptr_t>(static_cast<std::int64_t>(entry) + 5 + previous_rel));
  }
  if (!patchable) {
    return false; // unknown entry contents — refuse to overwrite blindly
  }
  const std::uintptr_t dst = reinterpret_cast<std::uintptr_t>(target);
  const std::int64_t rel = static_cast<std::int64_t>(dst) - static_cast<std::int64_t>(entry + 5);
  return rel <= 0x7FFF'FFFF && rel >= -0x8000'0000LL; // `jmp rel32` reach
}

bool code_pages::precheck_entry(std::uintptr_t entry, void* target) {
  return patchable_entry(entry, target);
}

bool code_pages::snapshot_entry(std::uintptr_t entry, std::uint8_t out[5]) {
  if (!patchable_entry(entry, reinterpret_cast<void*>(entry))) {
    return false; // do not even read entries we would refuse to write
  }
  std::memcpy(out, reinterpret_cast<const void*>(entry), 5);
  return true;
}

bool code_pages::patch_entry(std::uintptr_t entry, void* target) {
  if (!patchable_entry(entry, target)) {
    return false;
  }
  const std::uintptr_t dst = reinterpret_cast<std::uintptr_t>(target);
  const std::int64_t rel = static_cast<std::int64_t>(dst) - static_cast<std::int64_t>(entry + 5);

  // Flip the page writable, write E9 <rel32>, flip back to r-x.
  const std::uint64_t page = page_size();
  const std::uintptr_t page_start = entry & ~(page - 1);
  const std::uint64_t page_len = page * 2; // entry may straddle two pages
  if (mprotect(reinterpret_cast<void*>(page_start), page_len, PROT_READ | PROT_WRITE | PROT_EXEC) !=
      0) {
    return false;
  }
  auto* patch = reinterpret_cast<std::uint8_t*>(entry);
  patch[0] = 0xE9;
  std::memcpy(patch + 1, &rel, sizeof(std::int32_t));
  if (mprotect(reinterpret_cast<void*>(page_start), page_len, PROT_READ | PROT_EXEC) != 0) {
    // The redirect is written and executable, so the patch functionally
    // succeeded — but the page stays writable+executable, breaking W^X.
    // That degradation must be visible, not silent.
    neko::log(neko::log_level::error,
              "restoring r-x on a patched page failed (%s): page stays writable\n",
              std::strerror(errno));
  }
  __builtin___clear_cache(reinterpret_cast<char*>(entry), reinterpret_cast<char*>(entry + 5));
  return true;
}

bool code_pages::restore_entry(std::uintptr_t entry, const std::uint8_t original[5]) {
  const std::uint64_t page = page_size();
  const std::uintptr_t page_start = entry & ~(page - 1);
  const std::uint64_t page_len = page * 2; // entry may straddle two pages
  if (mprotect(reinterpret_cast<void*>(page_start), page_len, PROT_READ | PROT_WRITE | PROT_EXEC) !=
      0) {
    return false;
  }
  std::memcpy(reinterpret_cast<void*>(entry), original, 5);
  if (mprotect(reinterpret_cast<void*>(page_start), page_len, PROT_READ | PROT_EXEC) != 0) {
    neko::log(neko::log_level::error,
              "restoring r-x on a rolled-back page failed (%s): page stays writable\n",
              std::strerror(errno));
  }
  __builtin___clear_cache(reinterpret_cast<char*>(entry), reinterpret_cast<char*>(entry + 5));
  return true;
}

} // namespace neko::elf
