# Cross-TU candidate resolution: fresh objects call symbols defined by
# sibling objects of the same generation, including brand-new ones.
nekomata_add_e2e_test(
  NAME e2e.reload.cross_tu
  RUNNER cross_tu_runner
  SOURCES runner.cpp
  HOT_SOURCES a.cpp b.cpp
  SCRIPT cross_tu.sh.in
  TIMEOUT 180
  LABELS linux
  PRIVATE_INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src")
