nekomata_add_unit_test(
  NAME unit.backends.wasm.candidate
  SOURCES candidate.cpp
  LIBRARIES neko_wasm_candidate
  PRIVATE_INCLUDES "${PROJECT_SOURCE_DIR}/src")

nekomata_add_unit_test(
  NAME unit.backends.wasm.offer_poller
  SOURCES offer_poller.cpp
  LIBRARIES neko_wasm_candidate
  PRIVATE_INCLUDES "${PROJECT_SOURCE_DIR}/src")
