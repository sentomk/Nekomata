# Public headers live in include/neko; implementation headers stay private here.
add_library(neko STATIC
  core/log.cpp
  core/version.cpp
  runtime/depfile_planner.cpp
  runtime/descriptor_discovery.cpp
  runtime/descriptor_section.cpp
  runtime/generation.cpp
  runtime/generation_offer.cpp
  runtime/generation_stream.cpp
  runtime/group_descriptor.cpp
  runtime/session.cpp
  runtime/sha256.cpp)
add_library(nekomata::neko ALIAS neko)
target_include_directories(neko
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/generated>)
target_compile_features(neko PUBLIC cxx_std_20)
nekomata_apply_warnings(neko)
nekomata_apply_sanitizers(neko)
nekomata_apply_clang_tidy(neko)

add_subdirectory(platforms)
