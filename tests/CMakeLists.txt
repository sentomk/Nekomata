include(FetchContent)

# doctest is header-only, so we keep FetchContent from descending into the
# project's own CMakeLists.txt: SOURCE_SUBDIR points at a subdirectory that
# contains no CMakeLists.txt (its top-level cmake_minimum_required predates
# 3.10 and makes CMake >= 4 emit a deprecation warning on every configure).
FetchContent_Declare(
  doctest
  GIT_REPOSITORY https://github.com/doctest/doctest.git
  GIT_TAG v2.4.12
  SOURCE_SUBDIR doc)

FetchContent_MakeAvailable(doctest)

add_library(doctest INTERFACE)
target_include_directories(doctest INTERFACE "${doctest_SOURCE_DIR}")
add_library(doctest::doctest ALIAS doctest)

add_executable(hlr_smoke smoke.cpp)
target_link_libraries(hlr_smoke PRIVATE nekomata::hlr doctest::doctest)
nekomata_apply_warnings(hlr_smoke)
nekomata_apply_sanitizers(hlr_smoke)

add_test(NAME hlr.smoke COMMAND hlr_smoke)
