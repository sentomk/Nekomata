#include <neko/elf.hpp>
#include <neko/neko.hpp>

#include <type_traits>

namespace {

template <typename type>
concept complete_type = requires { sizeof(type); };

// The ordinary application entry must not pull in the backend extension SPI.
static_assert(!complete_type<neko::backend::bundle>);
static_assert(!complete_type<neko::backend::session_driver>);
static_assert(std::is_move_constructible_v<neko::backend_handle>);
static_assert(std::is_same_v<decltype(neko::elf::create_backend()), neko::backend_handle>);

#if defined(__EMSCRIPTEN__)
static_assert(std::is_same_v<decltype(neko::wasm::create_backend()), neko::backend_handle>);
#endif

} // namespace
