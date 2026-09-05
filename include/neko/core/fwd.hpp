// Lightweight declarations for the core module; no container definitions.
#pragma once

#include <cstdint>

namespace neko {

/// Opaque handle for a user-defined type known to the symbol backend.
using type_id = std::uint32_t;

struct function_info;
struct global_variable;
struct type_layout;
struct change_set;
struct patch_plan;

enum class log_level : std::uint8_t;

} // namespace neko
