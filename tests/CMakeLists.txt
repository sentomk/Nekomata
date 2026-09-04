# Tests use the vendored doctest single header (tests/vendor/doctest),
# so configuring never needs network access. Do not switch this back to
# FetchContent: dev machines and CI sandboxes behind restricted networks
# are a real scenario for this project (see commit "build: vendor doctest").
add_executable(neko_smoke smoke.cpp)
target_include_directories(neko_smoke
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/vendor")
target_link_libraries(neko_smoke PRIVATE nekomata::neko)
nekomata_apply_warnings(neko_smoke)
nekomata_apply_sanitizers(neko_smoke)

add_test(NAME neko.smoke COMMAND neko_smoke)

if(NEKOMATA_TARGET_LINUX)
  add_subdirectory(rejections)
endif()
