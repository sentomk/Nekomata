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

add_library(neko_tui STATIC monitor.cpp)
add_library(nekomata::tui ALIAS neko_tui)

target_link_libraries(neko_tui
  PUBLIC nekomata::neko
  PRIVATE glyph::glyph)

target_include_directories(neko_tui
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../include>)

nekomata_apply_warnings(neko_tui)
