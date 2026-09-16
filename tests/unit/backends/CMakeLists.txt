add_subdirectory(elf)
if(TARGET neko_backend_dwarf)
  add_subdirectory(dwarf)
endif()
