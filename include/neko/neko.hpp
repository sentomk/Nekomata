// Single-include entry for the nekomata library: the platform-neutral kernel
// plus the reload backend of the compiling platform.
//
// Deliberately not included:
//   - fwd.hpp: lean entry for compile-radius-sensitive TUs
#pragma once

#include <neko/config.hpp>

#include <neko/log.hpp>
#include <neko/session.hpp>
#include <neko/version.hpp>

// One factory header per platform backend; branches without a backend yet
// stay commented until the backend lands ("not supported yet").
#if defined(__EMSCRIPTEN__)
#include <neko/wasm.hpp>
#elif defined(__linux__)
#include <neko/elf.hpp>
#elif defined(_WIN32)
// #include <neko/pe.hpp>   // not supported yet
#endif

// The optional monitor joins only when the build provides it.
#if ENABLE_NEKOMATA_TUI
#include <neko/tui.hpp>
#endif
