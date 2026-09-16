#include <neko/fwd.hpp>

#include <type_traits>

namespace {

template <typename Type>
concept complete_type = requires { sizeof(Type); };

// Forward headers expose names without pulling in implementation definitions.
static_assert(std::is_same_v<neko::backend::type_id, std::uint32_t>);
static_assert(std::is_same_v<std::underlying_type_t<neko::log_level>, std::uint8_t>);
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
static_assert(!complete_type<neko::reload_session>);
static_assert(!complete_type<neko::backend::function_replacement>);
static_assert(!complete_type<neko::backend::loaded_image>);
static_assert(!complete_type<neko::backend::bundle>);
static_assert(!complete_type<neko::legacy::depfile_planner>);
static_assert(!complete_type<neko::legacy::depfile_entry>);
static_assert(!complete_type<neko::generation_watch>);

} // namespace
