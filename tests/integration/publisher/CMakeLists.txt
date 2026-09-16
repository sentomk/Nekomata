# The host publication primitive: consumer-side acceptance of what the
# publisher emits, plus the CLI end to end when the tool is built.
nekomata_add_unit_test(
  NAME integration.publisher
  SOURCES publisher.cpp
  PRIVATE_INCLUDES "${PROJECT_SOURCE_DIR}/src"
  LIBRARIES nekomata::neko neko_publisher_lib)

if(TARGET neko_publisher)
  add_test(NAME neko.integration.publisher.cli
    COMMAND "${CMAKE_COMMAND}"
      "-DPUBLISHER=$<TARGET_FILE:neko_publisher>"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/publisher_cli.cmake")
  set_tests_properties(neko.integration.publisher.cli PROPERTIES TIMEOUT 60
    LABELS integration)
endif()
