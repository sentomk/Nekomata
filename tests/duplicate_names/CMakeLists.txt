# neko.dupnames — duplicate-name matching regression (Linux only).
add_executable(dup_runner main.cpp a.cpp b.cpp)
target_link_libraries(dup_runner PRIVATE nekomata::backends::elf)
set_target_properties(dup_runner PROPERTIES POSITION_INDEPENDENT_CODE OFF)
target_compile_options(dup_runner PRIVATE -O0 -g -fno-pie -fno-pic)
target_link_options(dup_runner PRIVATE -no-pie)
set_source_files_properties(a.cpp b.cpp PROPERTIES
  COMPILE_OPTIONS "-fno-exceptions;-fno-asynchronous-unwind-tables")
nekomata_apply_warnings(dup_runner)

configure_file(dupnames.sh.in "${CMAKE_CURRENT_BINARY_DIR}/dupnames-run.sh" @ONLY)

if(TARGET nekomata AND TARGET neko_backend_dwarf)
  add_test(NAME neko.dupnames
    COMMAND bash "${CMAKE_CURRENT_BINARY_DIR}/dupnames-run.sh"
      --cli "$<TARGET_FILE:nekomata>")
else()
  add_test(NAME neko.dupnames
    COMMAND bash "${CMAKE_CURRENT_BINARY_DIR}/dupnames-run.sh")
endif()
set_tests_properties(neko.dupnames PROPERTIES TIMEOUT 180)
