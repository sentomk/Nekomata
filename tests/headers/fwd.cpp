#include <neko/core/fwd.hpp>
#include <neko/runtime/fwd.hpp>

#include <type_traits>

namespace {

template <typename Type>
concept complete_type = requires { sizeof(Type); };

// Forward headers expose names without pulling in implementation definitions.
static_assert(std::is_same_v<neko::type_id, std::uint32_t>);
static_assert(std::is_same_v<std::underlying_type_t<neko::log_level>, std::uint8_t>);
static_assert(!complete_type<neko::function_info>);
static_assert(!complete_type<neko::global_variable>);
static_assert(!complete_type<neko::type_layout>);
static_assert(!complete_type<neko::change_set>);
static_assert(!complete_type<neko::patch_plan>);
static_assert(!complete_type<neko::code_substituter>);
static_assert(!complete_type<neko::object_loader>);
static_assert(!complete_type<neko::patch_planner>);
static_assert(!complete_type<neko::state_manager>);
static_assert(!complete_type<neko::symbol_provider>);
static_assert(!complete_type<neko::reload_session>);
static_assert(!complete_type<neko::function_replacement>);
static_assert(!complete_type<neko::loaded_image>);
static_assert(!complete_type<neko::backend_bundle>);

} // namespace
