# Duplicate-name matching regression (Linux only). Debug info is a test
# input here: the disambiguation goes through the offline symbol manifest.
set(neko_dupnames_args "")
if(TARGET nekomata AND TARGET neko_backend_dwarf)
  set(neko_dupnames_args --cli "$<TARGET_FILE:nekomata>")
endif()
nekomata_add_e2e_test(
  NAME e2e.reload.duplicate_names
  RUNNER dup_runner
  SOURCES main.cpp
  HOT_SOURCES a.cpp b.cpp
  SCRIPT dupnames.sh.in
  DEBUG_INFO
  TIMEOUT 180
  LABELS linux
  TEST_ARGS ${neko_dupnames_args})
