# Managed watch end to end: embedded descriptor, published generations,
# unwatch/resume semantics against a real linker section.
nekomata_add_e2e_test(
  NAME e2e.reload.managed
  RUNNER managed_reload_runner
  SOURCES runner.cpp
  HOT_SOURCES managed_hot.cpp
  SCRIPT managed_reload.sh.in
  TIMEOUT 180
  LABELS linux
  PRIVATE_INCLUDES "${PROJECT_SOURCE_DIR}/src")
