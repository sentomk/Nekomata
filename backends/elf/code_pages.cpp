#include "code_pages.hpp"

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
    const std::uint64_t span = round_up(bytes, page_size());
    for (int step = 1; step <= 16; ++step) {
        for (const std::int64_t sign : {std::int64_t{1}, std::int64_t{-1}}) {
            const std::uintptr_t addr =
                hint + sign * static_cast<std::uintptr_t>(step) * 0x0800'0000ull;
            void* mapping = mmap(reinterpret_cast<void*>(addr), span, PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (mapping == MAP_FAILED) {
                continue;
            }
            if (within_rel32(reinterpret_cast<std::uintptr_t>(mapping), hint)) {
                return mapping;
            }
            munmap(mapping, span); // kernel placed it too far away
        }
    }
    return nullptr;
}

bool code_pages::commit_code(void* reservation, const void* image, std::uint64_t bytes) {
    if (reservation == nullptr || image == nullptr || bytes == 0) {
        return false;
    }
    const std::uint64_t span = round_up(bytes, page_size());
    std::memcpy(reservation, image, bytes);
    if (mprotect(reservation, span, PROT_READ | PROT_EXEC) != 0) {
        throw std::runtime_error(std::string("mprotect(PROT_EXEC) failed: ") +
                                 std::strerror(errno));
    }
    __builtin___clear_cache(static_cast<char*>(reservation),
                            static_cast<char*>(reservation) + bytes);
    return true;
}

bool code_pages::patch_entry(std::uintptr_t entry, void* target) {
    const auto* code = reinterpret_cast<const std::uint8_t*>(entry);
    const bool prologue_push_rbp =
        code[0] == 0x55 && code[1] == 0x48 && code[2] == 0x89 && code[3] == 0xE5;
    const bool prologue_endbr64 =
        code[0] == 0xF3 && code[1] == 0x0F && code[2] == 0x1E && code[3] == 0xFA && code[4] == 0x55;
    if (!prologue_push_rbp && !prologue_endbr64) {
        return false; // unknown prologue — refuse to overwrite blindly
    }

    const std::uintptr_t dst = reinterpret_cast<std::uintptr_t>(target);
    const std::int64_t rel = static_cast<std::int64_t>(dst) - static_cast<std::int64_t>(entry + 5);
    if (rel > 0x7FFF'FFFF || rel < -0x8000'0000LL) {
        return false; // out of `jmp rel32` range
    }

    // Flip the page writable, write E9 <rel32>, flip back to r-x.
    const std::uint64_t page = page_size();
    const std::uintptr_t page_start = entry & ~(page - 1);
    const std::uint64_t page_len = page * 2; // entry may straddle two pages
    if (mprotect(reinterpret_cast<void*>(page_start), page_len,
                 PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        return false;
    }
    auto* patch = reinterpret_cast<std::uint8_t*>(entry);
    patch[0] = 0xE9;
    std::memcpy(patch + 1, &rel, sizeof(std::int32_t));
    mprotect(reinterpret_cast<void*>(page_start), page_len, PROT_READ | PROT_EXEC);
    __builtin___clear_cache(reinterpret_cast<char*>(entry), reinterpret_cast<char*>(entry + 5));
    return true;
}

} // namespace neko::elf
