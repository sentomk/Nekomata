#include "code_pages.hpp"

#include <neko/log.hpp>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace neko::pe {
namespace {

std::uint64_t page_size() {
  static const std::uint64_t size = [] {
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    return info.dwPageSize > 0 ? static_cast<std::uint64_t>(info.dwPageSize) : std::uint64_t{4096};
  }();
  return size;
}

// VirtualAlloc reservation starts must honor the allocation granularity
// (64 KiB), not just the page size.
std::uint64_t allocation_granularity() {
  static const std::uint64_t size = [] {
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    return info.dwAllocationGranularity > 0
               ? static_cast<std::uint64_t>(info.dwAllocationGranularity)
               : std::uint64_t{64 * 1024};
  }();
  return size;
}

std::uint64_t round_up(std::uint64_t v, std::uint64_t align) {
  return (v + align - 1) / align * align;
}

/// ±2 GiB minus slack, so `jmp rel32` from anywhere in the old function
/// reaches anywhere in the new image.
constexpr std::uintptr_t rel32_limit = 0x7FFF'F000;
constexpr std::uint64_t default_pool_size = 64ull * 1024 * 1024;

bool within_rel32(std::uintptr_t a, std::uintptr_t b) {
  const auto delta = a > b ? a - b : b - a;
  return delta < rel32_limit;
}

bool range_within_rel32(std::uintptr_t begin, std::uintptr_t end, std::uintptr_t hint) {
  return begin < end && within_rel32(begin, hint) && within_rel32(end - 1, hint);
}

std::uintptr_t align_up_address(std::uintptr_t value, std::uintptr_t alignment) {
  const auto remainder = value % alignment;
  if (remainder == 0) {
    return value;
  }
  const auto addition = alignment - remainder;
  if (value > std::numeric_limits<std::uintptr_t>::max() - addition) {
    return std::numeric_limits<std::uintptr_t>::max();
  }
  return value + addition;
}

std::uintptr_t align_down_address(std::uintptr_t value, std::uintptr_t alignment) {
  return value - value % alignment;
}

struct mapped_range {
  std::uintptr_t begin;
  std::uintptr_t end;
};

struct pool_candidate {
  std::uintptr_t begin;
  std::uint64_t size;
  std::uintptr_t distance;
};

std::vector<mapped_range> free_ranges(std::uintptr_t window_begin, std::uintptr_t window_end) {
  std::vector<mapped_range> ranges;
  std::uintptr_t address = window_begin;
  while (address < window_end) {
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)) == 0) {
      break;
    }
    const auto region_begin = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
    const auto region_end = region_begin + info.RegionSize;
    if (info.State == MEM_FREE) {
      const auto begin = std::max<std::uintptr_t>(region_begin, window_begin);
      const auto end = std::min<std::uintptr_t>(region_end, window_end);
      if (begin < end) {
        ranges.push_back({begin, end});
      }
    }
    if (region_end <= address) {
      break; // defensive: never walk the same region twice
    }
    address = region_end;
  }
  return ranges;
}

std::vector<pool_candidate> find_pool_candidates(std::uintptr_t hint, std::uint64_t minimum_size) {
  const auto granularity = static_cast<std::uintptr_t>(allocation_granularity());
  const auto maximum = std::numeric_limits<std::uintptr_t>::max();
  auto window_begin = hint > rel32_limit ? hint - rel32_limit : granularity;
  auto window_end = hint < maximum - rel32_limit ? hint + rel32_limit : maximum;
  window_begin = align_up_address(window_begin, granularity);
  window_end = align_down_address(window_end, granularity);
  if (window_begin >= window_end || minimum_size > window_end - window_begin) {
    return {};
  }

  std::vector<pool_candidate> candidates;
  for (const auto& range : free_ranges(window_begin, window_end)) {
    const auto gap_begin = align_up_address(range.begin, granularity);
    const auto gap_end = align_down_address(range.end, granularity);
    if (gap_begin >= gap_end || minimum_size > gap_end - gap_begin) {
      continue;
    }

    const auto available = static_cast<std::uint64_t>(gap_end - gap_begin);
    const auto preferred = std::max(default_pool_size, minimum_size);
    const auto size = std::min(preferred, available);
    const auto ideal =
        hint > size / 2 ? align_down_address(hint - size / 2, granularity) : gap_begin;
    const auto latest = gap_end - size;
    const auto begin = std::clamp(ideal, gap_begin, latest);
    const auto end = begin + size;
    const auto distance = hint < begin ? begin - hint : (hint >= end ? hint - (end - 1) : 0);
    candidates.push_back({begin, size, distance});
  }

  std::sort(candidates.begin(), candidates.end(),
            [](const pool_candidate& left, const pool_candidate& right) {
              if (left.distance != right.distance) {
                return left.distance < right.distance;
              }
              return left.size > right.size;
            });
  return candidates;
}

/// The /Od prologue families an entry patch may consume; see code_pages.hpp.
bool arg_spill_prologue(const std::uint8_t* code) {
  std::size_t at = 0;
  std::uint8_t rex = 0;
  if (code[0] == 0x44 || code[0] == 0x48 || code[0] == 0x4C) {
    rex = code[0];
    at = 1;
  }
  if (code[at] != 0x89) { // mov r/m64, r64 (or r/m32, r32 without REX.W)
    return false;
  }
  const std::uint8_t modrm = code[at + 1];
  if ((modrm & 0xC7) != 0x44) { // mod=01 (disp8), rm=100 (SIB follows)
    return false;
  }
  if (code[at + 2] != 0x24) { // SIB: base=rsp, no index
    return false;
  }
  const std::uint8_t disp = code[at + 3];
  if (disp != 0x08 && disp != 0x10 && disp != 0x18 && disp != 0x20) {
    return false; // only the four register-argument home slots
  }
  const std::uint8_t reg = static_cast<std::uint8_t>(((rex & 0x4) ? 8u : 0u) | ((modrm >> 3) & 7));
  if ((rex & 0x4) == 0) {
    return reg == 1 || reg == 2; // ecx, edx
  }
  return reg == 8 || reg == 9; // r8d, r9d (REX.R extends the reg field)
}

bool sub_rsp_prologue(const std::uint8_t* code) {
  return (code[0] == 0x48 && code[1] == 0x83 && code[2] == 0xEC) || // sub rsp, imm8
         (code[0] == 0x48 && code[1] == 0x81 && code[2] == 0xEC);   // sub rsp, imm32
}

/// `push rbp; mov rbp,rsp`, optionally behind an endbr64. clang-cl omits
/// frame pointers by default (its /Od entries start with `sub rsp` above);
/// users enabling them via /Oy- or -fno-omit-frame-pointer get this family.
bool frame_pointer_prologue(const std::uint8_t* code) {
  const bool push_rbp = code[0] == 0x55 && code[1] == 0x48 && code[2] == 0x89 && code[3] == 0xE5;
  const bool endbr_push =
      code[0] == 0xF3 && code[1] == 0x0F && code[2] == 0x1E && code[3] == 0xFA && code[4] == 0x55;
  return push_rbp || endbr_push;
}

} // namespace

struct code_pages::allocation_state {
  struct slot {
    std::uintptr_t begin;
    std::uintptr_t usable_end;
    std::uintptr_t extent_end;
    bool active = true;
    bool process_lifetime = false;
    bool executable = false;
  };

  struct arena_pool {
    std::uintptr_t begin;
    std::uintptr_t end;
    std::uintptr_t cursor;
    std::vector<slot> slots;
  };

  ~allocation_state() {
    for (auto& pool : pools) {
      bool retained = false;
      for (const auto& candidate : pool.slots) {
        if (candidate.active && candidate.process_lifetime) {
          retained = true;
          break;
        }
      }
      if (!retained) {
        VirtualFree(reinterpret_cast<void*>(pool.begin), 0, MEM_RELEASE);
        continue;
      }
      // Slots redirected into by live code keep their pages; the rest of the
      // reservation stays reserved so the retained extents never move.
      for (const auto& candidate : pool.slots) {
        if (candidate.active && !candidate.process_lifetime) {
          VirtualFree(reinterpret_cast<void*>(candidate.begin),
                      candidate.extent_end - candidate.begin, MEM_DECOMMIT);
        }
      }
    }
  }

  [[nodiscard]] slot* find(std::uintptr_t begin) noexcept {
    for (auto& pool : pools) {
      for (auto& candidate : pool.slots) {
        if (candidate.active && candidate.begin == begin) {
          return &candidate;
        }
      }
    }
    return nullptr;
  }

  [[nodiscard]] const slot* find(std::uintptr_t begin) const noexcept {
    for (const auto& pool : pools) {
      for (const auto& candidate : pool.slots) {
        if (candidate.active && candidate.begin == begin) {
          return &candidate;
        }
      }
    }
    return nullptr;
  }

  [[nodiscard]] std::optional<std::uintptr_t> reserve(std::uintptr_t hint, std::uint64_t span) {
    for (auto& pool : pools) {
      for (auto& candidate : pool.slots) {
        if (candidate.active || span > candidate.extent_end - candidate.begin ||
            !range_within_rel32(candidate.begin, candidate.begin + span, hint)) {
          continue;
        }
        if (VirtualAlloc(reinterpret_cast<void*>(candidate.begin), static_cast<SIZE_T>(span),
                         MEM_COMMIT, PAGE_READWRITE) == nullptr) {
          continue;
        }
        candidate.usable_end = candidate.begin + span;
        candidate.active = true;
        candidate.process_lifetime = false;
        candidate.executable = false;
        return candidate.begin;
      }

      if (pool.cursor > pool.end || span > pool.end - pool.cursor ||
          !range_within_rel32(pool.cursor, pool.cursor + span, hint)) {
        continue;
      }
      if (VirtualAlloc(reinterpret_cast<void*>(pool.cursor), static_cast<SIZE_T>(span), MEM_COMMIT,
                       PAGE_READWRITE) == nullptr) {
        continue;
      }
      const auto begin = pool.cursor;
      pool.slots.push_back({begin, begin + span, begin + span, true, false, false});
      pool.cursor += span;
      return begin;
    }
    return std::nullopt;
  }

  void reclaim(std::uintptr_t begin) noexcept {
    auto* candidate = find(begin);
    if (candidate == nullptr) {
      return;
    }
    const auto extent = candidate->extent_end - candidate->begin;
    if (VirtualFree(reinterpret_cast<void*>(candidate->begin), static_cast<SIZE_T>(extent),
                    MEM_DECOMMIT) == 0) {
      candidate->process_lifetime = true;
      neko::log(neko::log_level::error,
                "decommitting a candidate code slot failed (%lu): reservation stays committed\n",
                GetLastError());
      return;
    }
    candidate->active = false;
    candidate->process_lifetime = false;
    candidate->executable = false;
    candidate->usable_end = candidate->begin;
  }

  void release_to_process(std::uintptr_t begin) noexcept {
    if (auto* candidate = find(begin)) {
      candidate->process_lifetime = true;
    }
  }

  std::vector<arena_pool> pools;
};

class code_pages::allocation final : public backend::executable_allocation,
                                     public backend::writable_allocation {
public:
  allocation(std::shared_ptr<allocation_state> state, std::uintptr_t begin, std::uint64_t size)
      : state_(std::move(state)), begin_(begin), size_(size) {}

  ~allocation() override {
    if (owns_mapping_) {
      if (reclaim_notify != nullptr) {
        reclaim_notify(reclaim_context);
      }
      state_->reclaim(begin_);
    }
  }

  void* data() noexcept override { return reinterpret_cast<void*>(begin_); }
  const void* data() const noexcept override { return reinterpret_cast<const void*>(begin_); }
  std::uint64_t size() const noexcept override { return size_; }

  void release_to_process() noexcept override {
    if (!owns_mapping_) {
      return;
    }
    reclaim_notify = nullptr; // released pages outlive every registration
    state_->release_to_process(begin_);
    owns_mapping_ = false;
  }

private:
  std::shared_ptr<allocation_state> state_;
  std::uintptr_t begin_;
  std::uint64_t size_;
  bool owns_mapping_ = true;

public:
  /// Fires when this handle reclaims the reservation (see on_reclaim()).
  void* reclaim_context = nullptr;
  void (*reclaim_notify)(void*) = nullptr;
};

code_pages::code_pages() : state_(std::make_shared<allocation_state>()) {}

code_pages::~code_pages() = default;

std::unique_ptr<code_pages::allocation> code_pages::reserve_near(std::uintptr_t hint,
                                                                 std::uint64_t bytes) {
  if (bytes == 0) {
    return nullptr;
  }
  const std::uint64_t page = page_size();
  if (bytes > std::numeric_limits<std::uint64_t>::max() - (page - 1)) {
    return nullptr;
  }
  const std::uint64_t span = round_up(bytes, page);
  if (span > std::numeric_limits<std::uint64_t>::max() - page) {
    return nullptr;
  }
  // Usable span + one spare page: the reservation's uncommitted remainder is
  // the guard that turns any overrun into a deterministic fault.
  const std::uint64_t total = span + page;

  if (const auto existing = state_->reserve(hint, span)) {
    try {
      return std::make_unique<allocation>(state_, *existing, bytes);
    } catch (...) {
      state_->reclaim(*existing);
      throw;
    }
  }

  for (const auto& candidate : find_pool_candidates(hint, total)) {
    void* mapping = VirtualAlloc(reinterpret_cast<void*>(candidate.begin),
                                 static_cast<SIZE_T>(candidate.size), MEM_RESERVE, PAGE_NOACCESS);
    if (mapping == nullptr) {
      continue;
    }
    if (reinterpret_cast<std::uintptr_t>(mapping) != candidate.begin) {
      // The allocator honored the request only approximately. Never accept a
      // surprise reservation: it may be out of rel32 range.
      VirtualFree(mapping, 0, MEM_RELEASE);
      continue;
    }

    try {
      state_->pools.push_back(
          {candidate.begin, candidate.begin + candidate.size, candidate.begin, {}});
    } catch (...) {
      VirtualFree(mapping, 0, MEM_RELEASE);
      throw;
    }

    try {
      const auto reserved = state_->reserve(hint, span);
      if (!reserved) {
        state_->pools.pop_back();
        VirtualFree(mapping, 0, MEM_RELEASE);
        continue;
      }
      try {
        return std::make_unique<allocation>(state_, *reserved, bytes);
      } catch (...) {
        state_->reclaim(*reserved);
        throw;
      }
    } catch (...) {
      if (!state_->pools.empty() && state_->pools.back().begin == candidate.begin &&
          state_->pools.back().slots.empty()) {
        state_->pools.pop_back();
        VirtualFree(mapping, 0, MEM_RELEASE);
      }
      throw;
    }
  }
  return nullptr;
}

backend::executable_allocation_ptr code_pages::reserve_code_near(std::uintptr_t hint,
                                                                 std::uint64_t bytes) {
  return reserve_near(hint, bytes);
}

backend::writable_allocation_ptr code_pages::reserve_writable_near(std::uintptr_t hint,
                                                                   std::uint64_t bytes) {
  return reserve_near(hint, bytes);
}

bool code_pages::owns_address(std::uintptr_t address) const {
  for (const auto& pool : state_->pools) {
    for (const auto& candidate : pool.slots) {
      if (candidate.active && candidate.executable && address >= candidate.begin &&
          address < candidate.usable_end) {
        return true;
      }
    }
  }
  return false;
}

bool code_pages::commit_code(backend::executable_allocation& reservation, const void* image,
                             std::uint64_t bytes) {
  if (reservation.data() == nullptr || image == nullptr || bytes == 0) {
    return false;
  }
  // Ownership + span invariant: the image must fit the reservation exactly
  // as it was sized. A larger image means the layout grew after the
  // reservation — reject loudly instead of writing past the span (the
  // uncommitted guard would catch it as a crash; this turns it into a
  // message).
  const auto begin = reinterpret_cast<std::uintptr_t>(reservation.data());
  const auto* slot = state_->find(begin);
  if (slot == nullptr) {
    throw std::runtime_error("commit_code: not a reservation made by this substituter");
  }
  if (bytes > reservation.size() || bytes > slot->usable_end - slot->begin) {
    throw std::runtime_error("code image (" + std::to_string(bytes) +
                             " bytes) exceeds its reservation (" +
                             std::to_string(reservation.size()) + " bytes) — refusing to commit");
  }
  std::memcpy(reservation.data(), image, static_cast<std::size_t>(bytes));
  DWORD previous = 0;
  if (VirtualProtect(reservation.data(), static_cast<SIZE_T>(slot->usable_end - slot->begin),
                     PAGE_EXECUTE_READ, &previous) == 0) {
    throw std::runtime_error("VirtualProtect(PAGE_EXECUTE_READ) failed with " +
                             std::to_string(GetLastError()));
  }
  FlushInstructionCache(GetCurrentProcess(), reservation.data(), static_cast<SIZE_T>(bytes));
  state_->find(begin)->executable = true;
  return true;
}

bool code_pages::patchable_entry(std::uintptr_t entry, void* target) const {
  const auto* code = reinterpret_cast<const std::uint8_t*>(entry);
  bool patchable =
      arg_spill_prologue(code) || sub_rsp_prologue(code) || frame_pointer_prologue(code);
  if (!patchable && code[0] == 0xE9) {
    // Possibly one of OUR previous redirects: an E9 whose target lands
    // inside a slot we committed can only have been written by us (an /Od
    // prologue never starts with a jmp, and an incremental-link thunk
    // targets module code instead). Re-patching in place is what makes
    // repeated reloads of the same function work.
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

namespace {

/// One page's original protection, saved while its contents are rewritten.
struct saved_protection {
  std::uintptr_t begin;
  DWORD protect;
};

/// Make [begin, begin+length) writable, recording each page's original
/// protection for make_executable_again. Returns false when a page cannot be
/// queried or flipped.
bool make_writable(std::uintptr_t begin, std::uint64_t length,
                   std::vector<saved_protection>& saved) {
  const auto page = static_cast<std::uintptr_t>(page_size());
  for (auto at = begin & ~(page - 1); at < begin + length; at += page) {
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(reinterpret_cast<void*>(at), &info, sizeof(info)) == 0) {
      return false;
    }
    DWORD previous = 0;
    if (VirtualProtect(reinterpret_cast<void*>(at), page, PAGE_EXECUTE_READWRITE, &previous) == 0) {
      return false;
    }
    saved.push_back({at, info.Protect});
  }
  return true;
}

void make_original_again(const std::vector<saved_protection>& saved) {
  for (const auto& page : saved) {
    DWORD previous = 0;
    if (VirtualProtect(reinterpret_cast<void*>(page.begin), 1, page.protect, &previous) == 0) {
      // The write is done and the page stays writable, breaking W^X. That
      // degradation must be visible, not silent.
      neko::log(neko::log_level::error,
                "restoring protection on a rewritten page failed (%lu): page stays writable\n",
                GetLastError());
    }
  }
}

} // namespace

bool code_pages::patch_entry(std::uintptr_t entry, void* target) {
  if (!patchable_entry(entry, target)) {
    return false;
  }
  const std::uintptr_t dst = reinterpret_cast<std::uintptr_t>(target);
  const std::int64_t rel = static_cast<std::int64_t>(dst) - static_cast<std::int64_t>(entry + 5);

  // Flip the covering pages writable, write E9 <rel32>, restore.
  std::vector<saved_protection> saved;
  if (!make_writable(entry, 5, saved)) {
    return false;
  }
  auto* patch = reinterpret_cast<std::uint8_t*>(entry);
  patch[0] = 0xE9;
  std::memcpy(patch + 1, &rel, sizeof(std::int32_t));
  make_original_again(saved);
  FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(entry), 5);
  return true;
}

bool code_pages::rewrite_reservation(backend::executable_allocation& reservation,
                                     std::uint64_t offset, const void* bytes, std::uint64_t size) {
  if (reservation.data() == nullptr || bytes == nullptr || size == 0 ||
      offset > reservation.size() || size > reservation.size() - offset) {
    return false;
  }
  const auto base = reinterpret_cast<std::uintptr_t>(reservation.data());
  const auto at = base + offset;
  const auto end = at + size;
  const auto* slot = state_->find(base);
  if (slot == nullptr || end > slot->usable_end) {
    return false;
  }
  std::vector<saved_protection> saved;
  if (!make_writable(at, size, saved)) {
    return false;
  }
  std::memcpy(reinterpret_cast<void*>(at), bytes, static_cast<std::size_t>(size));
  make_original_again(saved);
  FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(at),
                        static_cast<SIZE_T>(size));
  return true;
}

void code_pages::on_reclaim(backend::executable_allocation& reservation, void (*notify)(void*),
                            void* context) {
  auto& owned = static_cast<allocation&>(reservation);
  owned.reclaim_context = context;
  owned.reclaim_notify = notify;
}

bool code_pages::restore_entry(std::uintptr_t entry, const std::uint8_t original[5]) {
  std::vector<saved_protection> saved;
  if (!make_writable(entry, 5, saved)) {
    return false;
  }
  std::memcpy(reinterpret_cast<void*>(entry), original, 5);
  make_original_again(saved);
  FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(entry), 5);
  return true;
}

} // namespace neko::pe
