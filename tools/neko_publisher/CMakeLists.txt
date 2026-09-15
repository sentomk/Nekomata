# The private host publication executable that build adapters invoke after
# their native build produced a complete group. Not a supported CLI.
add_executable(neko_publisher publisher_main.cpp)
target_include_directories(neko_publisher PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(neko_publisher PRIVATE neko_publisher_lib)
nekomata_apply_warnings(neko_publisher)
nekomata_apply_sanitizers(neko_publisher)
