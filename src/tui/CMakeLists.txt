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
FetchContent_MakeAvailable(glyph)

if(WIN32)
  # The platform seam (terminal.hpp) has a POSIX implementation only; the
  # Win32 one — ConPTY plus the console's VT modes — is not written yet.
  # Failing loudly beats shipping a target that cannot work.
  message(FATAL_ERROR
    "NEKOMATA_TUI=ON is not supported on Windows yet (only the POSIX "
    "terminal backend exists). The rest of nekomata builds fine without it.")
endif()

add_library(neko_tui STATIC monitor.cpp terminal_posix.cpp)
add_library(nekomata::tui ALIAS neko_tui)

target_link_libraries(neko_tui
  PUBLIC nekomata::neko
  PRIVATE glyph::glyph)

target_include_directories(neko_tui
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../include>)

nekomata_apply_warnings(neko_tui)
