# Persistent new-global state across accepted, rejected, and sparse generations.
nekomata_add_e2e_test(
  NAME e2e.reload.new_global_state
  RUNNER new_global_state_runner
  SOURCES runner.cpp
  HOT_SOURCES a.cpp b.cpp
  SCRIPT new_global_state.sh.in
  TIMEOUT 240
  LABELS linux
  PRIVATE_INCLUDES "${PROJECT_SOURCE_DIR}/src")
