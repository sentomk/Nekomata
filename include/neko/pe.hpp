// pe backend — public factory for Windows reload sessions.

#pragma once

#include <neko/session.hpp>

namespace neko::pe {

/// Assemble the Windows/PE backend: DIA process symbols (also serving as
/// state manager), VirtualAlloc-based code pages and the object loader.
[[nodiscard]] backend_handle create_backend();

} // namespace neko::pe
