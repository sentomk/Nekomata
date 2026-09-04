# Examples. Phase 1 ships one Linux/ELF demo; more platforms follow their
# backend phases (Windows/PE with Phase 3).
if(NEKOMATA_TARGET_LINUX)
  add_subdirectory(hello_reload)
else()
  message(STATUS "nekomata: no examples for this platform (needs a backend)")
endif()
