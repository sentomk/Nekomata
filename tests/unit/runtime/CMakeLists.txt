nekomata_add_unit_test(
  NAME unit.runtime.generation_stream
  SOURCES generation_stream.cpp
  PRIVATE_INCLUDES "${PROJECT_SOURCE_DIR}/src"
  LIBRARIES nekomata::neko)

nekomata_add_unit_test(
  NAME unit.runtime.session_transaction
  SOURCES session_transaction.cpp
  PRIVATE_INCLUDES "${PROJECT_SOURCE_DIR}/src"
  LIBRARIES nekomata::neko)

nekomata_add_unit_test(
  NAME unit.runtime.managed_session
  SOURCES managed_session.cpp
  LIBRARIES nekomata::neko)

nekomata_add_unit_test(
  NAME unit.runtime.depfile_planner
  SOURCES depfile_planner.cpp
  LIBRARIES nekomata::neko)
