nekomata_add_unit_test(
  NAME integration.descriptor_discovery.core
  SOURCES descriptor_discovery.cpp
  PRIVATE_INCLUDES "${PROJECT_SOURCE_DIR}/src"
  LIBRARIES nekomata::neko)

# Requires the linker's __start_/__stop_ section markers, which are an ELF
# linking property.
if(NEKOMATA_TARGET_LINUX)
  nekomata_add_unit_test(
    NAME integration.descriptor_discovery.embedded
    SOURCES descriptor_embedded.cpp
    PRIVATE_INCLUDES "${PROJECT_SOURCE_DIR}/src"
    LIBRARIES nekomata::neko
    LABELS linux)
endif()
