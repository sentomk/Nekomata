// log.hpp — the shared diagnostics tag.
//
// One source of truth so every nekomata message wears the same cat.

#pragma once

namespace neko {

/// Prefix for nekomata's own diagnostics, e.g. "=^･ω･^= reload applied: ...".
inline constexpr const char* kLogTag = "=^･ω･^=";

} // namespace neko
