if(NEKOMATA_TARGET_LINUX)
  add_subdirectory(elf)
endif()
# The COFF reader suites parse bytes offline and run everywhere; the
# VirtualAlloc suite gates itself to Windows inside pe/.
add_subdirectory(pe)
if(TARGET neko_backend_dwarf)
  add_subdirectory(dwarf)
endif()
