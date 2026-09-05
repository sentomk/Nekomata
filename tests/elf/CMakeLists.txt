add_executable(neko_elf_file_tests binary_file.cpp)
target_include_directories(neko_elf_file_tests PRIVATE
  "${PROJECT_SOURCE_DIR}/tests/vendor" "${PROJECT_SOURCE_DIR}/src/platforms/elf")
target_link_libraries(neko_elf_file_tests PRIVATE neko_elf_file)
nekomata_apply_warnings(neko_elf_file_tests)
nekomata_apply_sanitizers(neko_elf_file_tests)
add_test(NAME neko.elf.file COMMAND neko_elf_file_tests)
set_tests_properties(neko.elf.file PROPERTIES TIMEOUT 30)
