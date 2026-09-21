# The executable supplies descriptor discovery, so the static runtime archive
# uses a portable registry instead of extracting its ELF discovery object.
# Session, worker, stream, preparation and commit all remain production code.
nekomata_add_unit_test(
  NAME integration.session.lifecycle
  SOURCES lifecycle.cpp
  PRIVATE_INCLUDES "${PROJECT_SOURCE_DIR}/src"
  LIBRARIES nekomata::neko)
