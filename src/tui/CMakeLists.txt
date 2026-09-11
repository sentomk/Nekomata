# neko::tui — terminal UI module. Optional; requires Glyph.
#
# The TUI renders a reload status panel using Glyph's Buffer + ANSI
# renderer. Off by default: -DNEKOMATA_TUI=ON to enable.
if(NOT NEKOMATA_TUI)
  return()
endif()

# Glyph is looked for first and downloaded only when it is missing, so a build
# that already has it installed — vcpkg, Conan, a distribution package — does
# not touch the network here. Set CMAKE_DISABLE_FIND_PACKAGE_glyph=ON to insist
# on the pinned revision instead.
find_package(glyph CONFIG QUIET)
if(NOT glyph_FOUND)
  include(FetchContent)
  # Pinned by commit: Glyph publishes no tags, and tracking a branch would make
  # this build depend on whatever landed upstream that morning. Move it by hand
  # when a Glyph change is wanted. GIT_SHALLOW needs a tag, so it cannot be used
  # with a commit — one more reason to tag a release.
  FetchContent_Declare(
    glyph
    GIT_REPOSITORY https://github.com/sentomk/Glyph.git
    GIT_TAG        53f80adef52affad75c56792a2396ecaa4402d6f)
  FetchContent_MakeAvailable(glyph)
endif()
# find_package() creates an imported target that is visible only in this
# directory and below, while FetchContent creates an ordinary one that is
# visible everywhere. The TUI's own tests live in a sibling directory and link
# glyph for its width table, so the imported case has to be promoted.
get_target_property(glyph_target_imported glyph::glyph IMPORTED)
if(glyph_target_imported)
  set_target_properties(glyph::glyph PROPERTIES IMPORTED_GLOBAL TRUE)
endif()

if(NOT TARGET glyph::glyph)
  message(FATAL_ERROR
    "NEKOMATA_TUI=ON but Glyph is not available: find_package did not find it "
    "and FetchContent did not populate it. Install it, put its prefix on "
    "CMAKE_PREFIX_PATH, or build without the TUI (-DNEKOMATA_TUI=OFF).")
endif()

# One transitive knob is still set from here, and only until the chain moves:
# the texere revision Glyph currently pins predates texere turning its own
# tests and tools off when embedded, and simdutf's `sutf` tool has to stay out
# on Windows — its iconv probe picks up msys2's mingw headers and contaminates
# an MSVC-ABI compile. Both belong to texere, and go there with the next Glyph
# pin bump.
set(SIMDUTF_TESTS OFF CACHE BOOL "" FORCE)
set(SIMDUTF_TOOLS OFF CACHE BOOL "" FORCE)

# The platform seam (terminal.hpp) is one file per platform. The Win32
# backend drives the current console through its VT modes; a separate
# terminal (ConPTY) is not supported there yet.
add_library(neko_tui STATIC monitor.cpp)
add_library(nekomata::tui ALIAS neko_tui)
if(WIN32)
  target_sources(neko_tui PRIVATE terminal_win32.cpp)
else()
  target_sources(neko_tui PRIVATE terminal_posix.cpp)
endif()

target_link_libraries(neko_tui
  PUBLIC nekomata::neko
  PRIVATE glyph::glyph)

target_include_directories(neko_tui
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../include>)

nekomata_apply_warnings(neko_tui)
