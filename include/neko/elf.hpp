// elf backend — public factory for Linux reload sessions.

#pragma once

#include <neko/session.hpp>

namespace neko::elf {

/// Assemble the Linux/ELF backend: process symbols (also serving as
/// state manager), mmap-based code pages and the object loader.
[[nodiscard]] backend_handle create_backend();

} // namespace neko::elf
