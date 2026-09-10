# TUI tests. The pty-driven suites need a pty and fork, so they are
# Unix-only; the TUI itself is off unless -DNEKOMATA_TUI=ON.

# Pure layout: no terminal, so it runs anywhere the TUI is built.
add_executable(neko_tui_layout layout.cpp)
target_include_directories(neko_tui_layout
  PRIVATE "${PROJECT_SOURCE_DIR}/tests/vendor")
target_link_libraries(neko_tui_layout PRIVATE nekomata::tui glyph::glyph)
nekomata_apply_warnings(neko_tui_layout)
nekomata_apply_sanitizers(neko_tui_layout)
add_test(NAME neko.tui.layout COMMAND neko_tui_layout)
set_tests_properties(neko.tui.layout PROPERTIES TIMEOUT 15)

# Everything below drives a real pty in real time: Unix-only.
if(WIN32)
  return()
endif()

add_executable(neko_tui_monitor_input monitor_input.cpp)
target_include_directories(neko_tui_monitor_input
  PRIVATE "${PROJECT_SOURCE_DIR}/tests/vendor")
# glyph::glyph is linked so the tests can use the same width rule the
# renderer uses (neko_tui keeps Glyph private).
target_link_libraries(neko_tui_monitor_input PRIVATE nekomata::tui glyph::glyph)
nekomata_apply_warnings(neko_tui_monitor_input)
nekomata_apply_sanitizers(neko_tui_monitor_input)
add_test(NAME neko.tui.monitor_input COMMAND neko_tui_monitor_input)
# The suite drives a pty in real time; generous but bounded.
set_tests_properties(neko.tui.monitor_input PROPERTIES TIMEOUT 60)
