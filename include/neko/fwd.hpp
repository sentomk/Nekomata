// Public forward declarations without complete type definitions.
#pragma once

#include <cstdint>

namespace neko {

class reload_session;
struct generation_watch;

enum class log_level : std::uint8_t;

namespace backend {

using type_id = std::uint32_t;

class object_loader;
class symbol_provider;
class state_manager;
class code_substituter;
class patch_planner;

struct bundle;
struct function_info;
struct global_variable;
struct type_layout;
struct change_set;
struct patch_plan;
struct function_replacement;
struct loaded_image;

} // namespace backend

namespace legacy {

class depfile_planner;
struct depfile_entry;

} // namespace legacy

} // namespace neko
