# The private C substrate: leaf primitives without domain knowledge.
#
# Built with the C compiler, never installed, and not a public commitment
# (moving anything here to include/neko would make it one). The C++ wrappers
# next to the c/ directory ride this target's interface include so internal
# modules can `#include <base/sha256.hpp>` against the src/ root.
add_library(neko_base_c STATIC c/neko_sha256.c)
if(WIN32)
  target_sources(neko_base_c PRIVATE
    c/neko_file_win32.c
    c/neko_lock_win32.c)
else()
  target_sources(neko_base_c PRIVATE
    c/neko_file_posix.c
    c/neko_lock_posix.c)
endif()
set_target_properties(neko_base_c PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON)
target_include_directories(neko_base_c
  INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>)

if(MSVC)
  target_compile_options(neko_base_c PRIVATE /W4)
  if(NEKOMATA_WARNINGS_AS_ERRORS)
    target_compile_options(neko_base_c PRIVATE /WX)
  endif()
else()
  set(neko_base_c_flags -Wall -Wextra -Wpedantic)
  if(NEKOMATA_WARNINGS_AS_ERRORS)
    list(APPEND neko_base_c_flags -Werror)
  endif()
  target_compile_options(neko_base_c PRIVATE ${neko_base_c_flags})
endif()
nekomata_apply_sanitizers(neko_base_c)
nekomata_apply_clang_tidy(neko_base_c)
