// elf backend — public factory assembling the Linux backend bundle.

#pragma once

#include <neko/runtime/session.hpp>

namespace neko::elf {

/// Assemble the Phase 1 Linux/ELF backend: process symbols (also serving as
/// state manager), mmap-based code pages and the object loader.
neko::backend_bundle create_backend();

} // namespace neko::elf
