# Real high-cardinality generations. Each runner links exactly the number of
# baseline translation units named by its descriptor; the script republishes
# the same complete membership at runtime.
set(neko_cross_tu_scale_generated_dir "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${neko_cross_tu_scale_generated_dir}")

set(NEKO_SCALE_INDEX 0)
configure_file(root.cpp.in "${neko_cross_tu_scale_generated_dir}/unit_0.cpp" @ONLY)
set(neko_cross_tu_scale_sources "${neko_cross_tu_scale_generated_dir}/unit_0.cpp")
foreach(NEKO_SCALE_INDEX RANGE 1 63)
  configure_file(unit.cpp.in
    "${neko_cross_tu_scale_generated_dir}/unit_${NEKO_SCALE_INDEX}.cpp" @ONLY)
  list(APPEND neko_cross_tu_scale_sources
    "${neko_cross_tu_scale_generated_dir}/unit_${NEKO_SCALE_INDEX}.cpp")
endforeach()

foreach(NEKO_SCALE_TU_COUNT IN ITEMS 16 32 64)
  list(SUBLIST neko_cross_tu_scale_sources 0 ${NEKO_SCALE_TU_COUNT} neko_scale_sources)
  set(neko_scale_runner "cross_tu_scale_${NEKO_SCALE_TU_COUNT}_runner")
  nekomata_add_e2e_test(
    NAME "e2e.reload.cross_tu_scale_${NEKO_SCALE_TU_COUNT}"
    RUNNER "${neko_scale_runner}"
    SOURCES runner.cpp
    HOT_SOURCES ${neko_scale_sources}
    SCRIPT cross_tu_scale.sh.in
    TIMEOUT 240
    LABELS linux slow
    PRIVATE_INCLUDES "${PROJECT_SOURCE_DIR}/src")
  target_compile_definitions("${neko_scale_runner}" PRIVATE
    NEKO_SCALE_TU_COUNT=${NEKO_SCALE_TU_COUNT})
endforeach()
