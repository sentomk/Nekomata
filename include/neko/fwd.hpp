// Public forward declarations without complete type definitions.
#pragma once

#include <cstdint>

namespace neko {

class reload_session;

enum class log_level : std::uint8_t;

namespace backend {

using type_id = std::uint32_t;

enum class generation_symbol_kind : std::uint8_t;
enum class generation_fixup_kind : std::uint8_t;

class object_loader;
class symbol_provider;
class state_manager;
class code_substituter;
class patch_planner;
class session_driver;

struct bundle;
struct function_info;
struct global_variable;
struct type_layout;
struct change_set;
struct patch_plan;
struct function_replacement;
struct generation_symbol;
struct generation_fixup;
struct loaded_image;

} // namespace backend

} // namespace neko
