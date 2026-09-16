add_executable(fuzz_parse_object elf_object/fuzz_parse_object.cpp)
target_include_directories(fuzz_parse_object PRIVATE
  "${PROJECT_SOURCE_DIR}/src/backends/elf/format"
  "${PROJECT_SOURCE_DIR}/src/backends/elf/runtime")
target_link_libraries(fuzz_parse_object PRIVATE neko_backend_elf)
