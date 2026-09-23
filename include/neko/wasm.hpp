#pragma once

#include <neko/backend/session_driver.hpp>

#include <memory>

namespace neko::wasm {
#if defined(__EMSCRIPTEN__)
/// Discover build-generated groups, initially disabled, and own their browser
/// transport, scheduling and preparation. Pass the result to reload_session.
/// Only one browser backend may exist per page because PLT slots are global.
[[nodiscard]] std::unique_ptr<backend::session_driver> create_backend();
#endif
} // namespace neko::wasm
