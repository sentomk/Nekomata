# One caller, three brand-new symbols from three sibling objects — three
# pending cross-object call fixups resolved by one link_generation().
nekomata_add_e2e_test(
  NAME e2e.reload.cross_tu_multi
  RUNNER cross_tu_multi_runner
  SOURCES runner.cpp
  HOT_SOURCES a.cpp b.cpp c.cpp d.cpp
  SCRIPT cross_tu_multi.sh.in
  TIMEOUT 180
  LABELS linux
  PRIVATE_INCLUDES "${PROJECT_SOURCE_DIR}/src")
