// elf backend — public factory assembling the Linux backend bundle.

#pragma once

#include <neko/backend.hpp>
#include <neko/session.hpp>

namespace neko::elf {

/// Assemble the Linux/ELF backend: process symbols (also serving as
/// state manager), mmap-based code pages and the object loader.
backend::bundle create_backend();

} // namespace neko::elf
