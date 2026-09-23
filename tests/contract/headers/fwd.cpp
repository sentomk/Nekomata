#include <neko/fwd.hpp>

#include <type_traits>

namespace {

template <typename Type>
concept complete_type = requires { sizeof(Type); };

// Forward headers expose names without pulling in implementation definitions.
static_assert(std::is_same_v<neko::backend::type_id, std::uint32_t>);
static_assert(std::is_same_v<std::underlying_type_t<neko::log_level>, std::uint8_t>);
static_assert(
    std::is_same_v<std::underlying_type_t<neko::backend::generation_symbol_kind>, std::uint8_t>);
static_assert(
    std::is_same_v<std::underlying_type_t<neko::backend::generation_fixup_kind>, std::uint8_t>);
static_assert(!complete_type<neko::backend::function_info>);
static_assert(!complete_type<neko::backend::global_variable>);
static_assert(!complete_type<neko::backend::type_layout>);
static_assert(!complete_type<neko::backend::change_set>);
static_assert(!complete_type<neko::backend::patch_plan>);
static_assert(!complete_type<neko::backend::code_substituter>);
static_assert(!complete_type<neko::backend::object_loader>);
static_assert(!complete_type<neko::backend::patch_planner>);
static_assert(!complete_type<neko::backend::state_manager>);
static_assert(!complete_type<neko::backend::symbol_provider>);
static_assert(!complete_type<neko::backend::handle_factory>);
static_assert(!complete_type<neko::reload_session>);
static_assert(!complete_type<neko::backend_handle>);
static_assert(!complete_type<neko::backend::function_replacement>);
static_assert(!complete_type<neko::backend::generation_symbol>);
static_assert(!complete_type<neko::backend::generation_fixup>);
static_assert(!complete_type<neko::backend::loaded_image>);
static_assert(!complete_type<neko::backend::bundle>);

} // namespace
