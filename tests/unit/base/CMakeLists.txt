nekomata_add_unit_test(
  NAME unit.base.sha256
  SOURCES sha256.cpp
  PRIVATE_INCLUDES "${PROJECT_SOURCE_DIR}/src"
  LIBRARIES nekomata::neko)
