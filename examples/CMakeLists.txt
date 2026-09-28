# Examples. Today one Linux/ELF demo ships; more platforms follow
# their backends (Windows/PE, planned). The browser demos, wasm_reload and
# flock, are standalone package consumers built by their own scripts.
if(NEKOMATA_TARGET_LINUX)
  add_subdirectory(hello_reload)
else()
  message(STATUS "nekomata: no examples for this platform (needs a backend)")
endif()
