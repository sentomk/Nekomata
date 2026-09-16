# Multi-generation reload soak (Linux only). The driver script drops
# randomized logic variants into one live process; the runner proves
# per-generation state continuity and reports allocation accounting from
# its own executable slot (r-xp region + PROT_NONE guard) inside a pool.
nekomata_add_e2e_test(
  NAME e2e.reload.soak
  RUNNER soak_runner
  SOURCES runner.cpp
  HOT_SOURCES hot.cpp
  SCRIPT soak.sh.in
  TIMEOUT 180
  LABELS linux slow)
