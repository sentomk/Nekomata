# Atomic patching regression (Linux only).
nekomata_add_e2e_test(
  NAME e2e.reload.atomicity
  RUNNER atomic_runner
  SOURCES runner.cpp
  HOT_SOURCES hot.cpp
  SCRIPT atomicity.sh.in
  TIMEOUT 180
  LABELS linux)
