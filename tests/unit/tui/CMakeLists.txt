# TUI tests. The pty-driven suite needs a pty and fork, so it is Unix-only;
# the TUI itself is off unless -DNEKOMATA_TUI=ON.

# Pure layout: no terminal, so it runs anywhere the TUI is built.
nekomata_add_unit_test(
  NAME unit.tui.layout
  SOURCES layout.cpp
  PRIVATE_INCLUDES "${PROJECT_SOURCE_DIR}/src"
  LIBRARIES nekomata::tui glyph::glyph
  TIMEOUT 15
  LABELS tui)

# Everything below drives a real pty in real time: Unix-only.
if(WIN32)
  return()
endif()

# glyph::glyph is linked so the test can use the same width rule the
# renderer uses (neko_tui keeps Glyph private).
nekomata_add_unit_test(
  NAME unit.tui.monitor_input
  SOURCES monitor_input.cpp
  PRIVATE_INCLUDES "${PROJECT_SOURCE_DIR}/src"
  LIBRARIES nekomata::tui glyph::glyph
  TIMEOUT 60
  LABELS tui)
