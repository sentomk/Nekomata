# neko::tui — terminal UI module. Optional; requires Glyph.
#
# The TUI renders a reload status panel using Glyph's Buffer + ANSI
# renderer. Off by default: -DNEKOMATA_TUI=ON to enable.
if(NOT NEKOMATA_TUI)
  return()
endif()

include(FetchContent)
FetchContent_Declare(
  glyph
  GIT_REPOSITORY https://github.com/sentomk/Glyph.git
  GIT_TAG        main)

# Transitive knobs: Glyph pulls texere, which pulls simdutf/uni_algo, and
# their tools and tests are not ours to build. simdutf's sutf tool is
# actively harmful on Windows: its iconv probe picks up msys2's mingw
# headers and contaminates an MSVC-ABI compile.
set(SIMDUTF_TESTS OFF CACHE BOOL "" FORCE)
set(SIMDUTF_TOOLS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(glyph)

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
