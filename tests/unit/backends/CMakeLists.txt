if(NEKOMATA_TARGET_LINUX)
  add_subdirectory(elf)
endif()
# The COFF reader suites parse bytes offline and run everywhere; the
# VirtualAlloc suite gates itself to Windows inside pe/.
add_subdirectory(pe)
if(TARGET neko_backend_pdb)
  # The DIA reader is real only when the SDK was found; the placeholder
  # target carries nothing to test.
  get_target_property(neko_pdb_type neko_backend_pdb TYPE)
  if(NOT neko_pdb_type STREQUAL "INTERFACE_LIBRARY")
    add_subdirectory(pdb)
  endif()
endif()
if(TARGET neko_backend_dwarf)
  add_subdirectory(dwarf)
endif()
