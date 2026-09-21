nekomata_add_unit_test(
  NAME unit.protocol.group_descriptor
  SOURCES group_descriptor.cpp
  PRIVATE_INCLUDES "${PROJECT_SOURCE_DIR}/src"
  LIBRARIES nekomata::neko)

nekomata_add_unit_test(
  NAME unit.protocol.generation_offer
  SOURCES generation_offer.cpp
  PRIVATE_INCLUDES "${PROJECT_SOURCE_DIR}/src"
  LIBRARIES nekomata::neko)

nekomata_add_unit_test(
  NAME unit.protocol.descriptor_section
  SOURCES descriptor_section.cpp
  PRIVATE_INCLUDES "${PROJECT_SOURCE_DIR}/src"
  LIBRARIES nekomata::neko)
