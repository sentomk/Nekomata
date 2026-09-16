#include "code_pages.hpp"

#include <neko/log.hpp>

#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

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
constexpr std::uintptr_t rel32_limit = 0x7FFF'F000;
constexpr std::uint64_t default_pool_size = 64ull * 1024 * 1024;

#ifdef MAP_FIXED_NOREPLACE
constexpr int map_fixed_no_replace = MAP_FIXED_NOREPLACE;
#else
// Linux 4.17 assigned this stable UAPI value. Older kernels may ignore an
// unknown flag and treat the address as a hint, so callers must still verify
// that mmap returned the requested address.
constexpr int map_fixed_no_replace = 0x100000;
#endif

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

std::uintptr_t minimum_mappable_address() {
  static const auto minimum = [] {
    // 64 KiB is Linux's common security floor. Prefer a conservative value
    // if the sysctl is hidden by a container rather than proposing an address
    // the kernel will reject with EPERM.
    std::uint64_t configured = 64 * 1024;
    std::ifstream setting("/proc/sys/vm/mmap_min_addr");
    if (setting) {
      setting >> configured;
    }
    const auto page = static_cast<std::uintptr_t>(page_size());
    const auto bounded = configured > std::numeric_limits<std::uintptr_t>::max()
                             ? std::numeric_limits<std::uintptr_t>::max()
                             : static_cast<std::uintptr_t>(configured);
    return align_up_address(std::max(page, bounded), page);
  }();
  return minimum;
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

std::vector<mapped_range> process_mappings() {
  std::ifstream maps("/proc/self/maps");
  if (!maps) {
    return {};
  }

  std::vector<mapped_range> ranges;
  std::string line;
  while (std::getline(maps, line)) {
    mapped_range range{};
    if (std::sscanf(line.c_str(), "%" SCNxPTR "-%" SCNxPTR, &range.begin, &range.end) == 2 &&
        range.begin < range.end) {
      ranges.push_back(range);
    }
  }
  std::sort(ranges.begin(), ranges.end(), [](const mapped_range& left, const mapped_range& right) {
    return left.begin < right.begin;
  });
  return ranges;
}

std::vector<pool_candidate> find_pool_candidates(std::uintptr_t hint, std::uint64_t minimum_size) {
  const auto page = static_cast<std::uintptr_t>(page_size());
  const auto maximum = std::numeric_limits<std::uintptr_t>::max();
  const auto address_floor = minimum_mappable_address();
  auto window_begin = hint > rel32_limit ? hint - rel32_limit : address_floor;
  auto window_end = hint < maximum - rel32_limit ? hint + rel32_limit : maximum;
  window_begin = align_up_address(std::max(window_begin, address_floor), page);
  window_end = align_down_address(window_end, page);
  if (window_begin >= window_end || minimum_size > window_end - window_begin) {
    return {};
  }

  std::vector<pool_candidate> candidates;
  const auto add_gap = [&](std::uintptr_t gap_begin, std::uintptr_t gap_end) {
    gap_begin = align_up_address(gap_begin, page);
    gap_end = align_down_address(gap_end, page);
    if (gap_begin >= gap_end || minimum_size > gap_end - gap_begin) {
      return;
    }

    const auto available = static_cast<std::uint64_t>(gap_end - gap_begin);
    const auto preferred = std::max(default_pool_size, minimum_size);
    const auto size = std::min(preferred, available);
    const auto ideal = hint > size / 2 ? align_down_address(hint - size / 2, page) : gap_begin;
    const auto latest = gap_end - size;
    const auto begin = std::clamp(ideal, gap_begin, latest);
    const auto end = begin + size;
    const auto distance = hint < begin ? begin - hint : (hint >= end ? hint - (end - 1) : 0);
    candidates.push_back({begin, size, distance});
  };

  auto cursor = window_begin;
  for (const auto& mapping : process_mappings()) {
    if (mapping.end <= cursor) {
      continue;
    }
    if (mapping.begin >= window_end) {
      break;
    }
    if (mapping.begin > cursor) {
      add_gap(cursor, std::min(mapping.begin, window_end));
    }
    cursor = std::max(cursor, align_up_address(mapping.end, page));
    if (cursor >= window_end) {
      break;
    }
  }
  if (cursor < window_end) {
    add_gap(cursor, window_end);
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

} // namespace

struct code_pages::allocation_state {
  struct slot {
    std::uintptr_t begin;
    std::uintptr_t usable_end;
    std::uintptr_t extent_end;
    bool active = true;
    bool process_lifetime = false;
  };

  struct arena_pool {
    std::uintptr_t begin;
    std::uintptr_t end;
    std::uintptr_t cursor;
    std::vector<slot> slots;
  };

  ~allocation_state() {
    for (const auto& pool : pools) {
      auto retained_begin = pool.end;
      auto retained_end = pool.begin;
      for (const auto& candidate : pool.slots) {
        if (!candidate.active || !candidate.process_lifetime) {
          continue;
        }
        retained_begin = std::min(retained_begin, candidate.begin);
        retained_end = std::max(retained_end, candidate.extent_end);
      }
      if (retained_begin == pool.end) {
        munmap(reinterpret_cast<void*>(pool.begin), pool.end - pool.begin);
        continue;
      }
      if (pool.begin < retained_begin) {
        munmap(reinterpret_cast<void*>(pool.begin), retained_begin - pool.begin);
      }
      if (retained_end < pool.end) {
        munmap(reinterpret_cast<void*>(retained_end), pool.end - retained_end);
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
    const auto page = page_size();
    const auto total = span + page;
    for (auto& pool : pools) {
      for (auto& candidate : pool.slots) {
        if (candidate.active || total > candidate.extent_end - candidate.begin ||
            !range_within_rel32(candidate.begin, candidate.begin + span, hint)) {
          continue;
        }
        if (mprotect(reinterpret_cast<void*>(candidate.begin), span, PROT_READ | PROT_WRITE) != 0) {
          continue;
        }
        candidate.usable_end = candidate.begin + span;
        candidate.active = true;
        candidate.process_lifetime = false;
        return candidate.begin;
      }

      if (pool.cursor > pool.end || total > pool.end - pool.cursor ||
          !range_within_rel32(pool.cursor, pool.cursor + span, hint)) {
        continue;
      }
      pool.slots.reserve(pool.slots.size() + 1);
      if (mprotect(reinterpret_cast<void*>(pool.cursor), span, PROT_READ | PROT_WRITE) != 0) {
        continue;
      }
      const auto begin = pool.cursor;
      pool.slots.push_back({begin, begin + span, begin + total});
      pool.cursor += total;
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
    if (mprotect(reinterpret_cast<void*>(candidate->begin), extent, PROT_NONE) != 0) {
      candidate->process_lifetime = true;
      neko::log(neko::log_level::error,
                "reclaiming a candidate code slot failed (%s): reservation stays mapped\n",
                std::strerror(errno));
      return;
    }
    static_cast<void>(madvise(reinterpret_cast<void*>(candidate->begin), extent, MADV_DONTNEED));
    candidate->active = false;
    candidate->process_lifetime = false;
    candidate->usable_end = candidate->begin;
  }

  void release_to_process(std::uintptr_t begin) noexcept {
    if (auto* candidate = find(begin)) {
      candidate->process_lifetime = true;
    }
  }

  std::vector<arena_pool> pools;
};

class code_pages::allocation final : public backend::executable_allocation {
public:
  allocation(std::shared_ptr<allocation_state> state, std::uintptr_t begin, std::uint64_t size)
      : state_(std::move(state)), begin_(begin), size_(size) {}

  ~allocation() override {
    if (owns_mapping_) {
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
    state_->release_to_process(begin_);
    owns_mapping_ = false;
  }

private:
  std::shared_ptr<allocation_state> state_;
  std::uintptr_t begin_;
  std::uint64_t size_;
  bool owns_mapping_ = true;
};

code_pages::code_pages() : state_(std::make_shared<allocation_state>()) {}

code_pages::~code_pages() = default;

backend::executable_allocation_ptr code_pages::reserve_code_near(std::uintptr_t hint,
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
  const std::uint64_t total = span + page; // usable span + PROT_NONE guard page

  if (const auto existing = state_->reserve(hint, span)) {
    try {
      return std::make_unique<allocation>(state_, *existing, bytes);
    } catch (...) {
      state_->reclaim(*existing);
      throw;
    }
  }

  for (const auto& candidate : find_pool_candidates(hint, total)) {
    void* mapping = mmap(reinterpret_cast<void*>(candidate.begin), candidate.size, PROT_NONE,
                         MAP_PRIVATE | MAP_ANONYMOUS | map_fixed_no_replace, -1, 0);
    if (mapping == MAP_FAILED) {
      continue;
    }
    if (reinterpret_cast<std::uintptr_t>(mapping) != candidate.begin) {
      // An old kernel ignored MAP_FIXED_NOREPLACE and treated the address as
      // a hint. Never accept a surprise mapping: it may be out of rel32 range.
      munmap(mapping, candidate.size);
      continue;
    }

    try {
      state_->pools.push_back(
          {candidate.begin, candidate.begin + candidate.size, candidate.begin, {}});
    } catch (...) {
      munmap(mapping, candidate.size);
      throw;
    }

    try {
      const auto reserved = state_->reserve(hint, span);
      if (!reserved) {
        state_->pools.pop_back();
        munmap(mapping, candidate.size);
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
        munmap(mapping, candidate.size);
      }
      throw;
    }
  }
  return nullptr;
}

bool code_pages::owns_address(std::uintptr_t address) const {
  for (const auto& pool : state_->pools) {
    for (const auto& candidate : pool.slots) {
      if (candidate.active && address >= candidate.begin && address < candidate.usable_end) {
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
  // guard page would catch it as a crash; this turns it into a message).
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
  std::memcpy(reservation.data(), image, bytes);
  if (mprotect(reservation.data(), slot->usable_end - slot->begin, PROT_READ | PROT_EXEC) != 0) {
    throw std::runtime_error(std::string("mprotect(PROT_EXEC) failed: ") + std::strerror(errno));
  }
  __builtin___clear_cache(static_cast<char*>(reservation.data()),
                          static_cast<char*>(reservation.data()) + bytes);
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
  const std::uint64_t page = page_size();
  const std::uintptr_t page_start = at & ~(page - 1);
  const std::uint64_t page_len = ((end - 1) & ~(page - 1)) - page_start + page;
  if (mprotect(reinterpret_cast<void*>(page_start), page_len, PROT_READ | PROT_WRITE | PROT_EXEC) !=
      0) {
    return false;
  }
  std::memcpy(reinterpret_cast<void*>(at), bytes, static_cast<std::size_t>(size));
  if (mprotect(reinterpret_cast<void*>(page_start), page_len, PROT_READ | PROT_EXEC) != 0) {
    neko::log(neko::log_level::error,
              "restoring r-x on a rewritten image page failed (%s): page stays writable\n",
              std::strerror(errno));
  }
  __builtin___clear_cache(reinterpret_cast<char*>(at), reinterpret_cast<char*>(end));
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
