# One atomic transaction spanning two object files (Linux only).
nekomata_add_e2e_test(
  NAME e2e.reload.multi_tu
  RUNNER multi_tu_runner
  SOURCES runner.cpp
  HOT_SOURCES a.cpp b.cpp
  SCRIPT multi_tu.sh.in
  TIMEOUT 180
  LABELS linux)
