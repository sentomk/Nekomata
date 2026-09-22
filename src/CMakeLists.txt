# Public headers live in include/neko; implementation headers stay private here.
#
# The target split states the boundaries in the link graph:
#   neko_protocol        the generation codecs shared by runtime and publisher
#   neko_runtime         the consumer kernel (session, streams, object watches)
#   neko_publisher_lib   the publication primitive behind the host publisher
# The convenience aggregate behind the `nekomata::neko` alias is defined in
# the top-level CMakeLists once this directory has returned.

add_subdirectory(base)

add_library(neko_protocol STATIC
  protocol/group_descriptor.cpp
  protocol/generation_offer.cpp
  protocol/wasm_offer.cpp
  protocol/descriptor_section.cpp)
target_include_directories(neko_protocol PRIVATE "${PROJECT_SOURCE_DIR}/src")
nekomata_apply_warnings(neko_protocol)
nekomata_apply_sanitizers(neko_protocol)
nekomata_apply_clang_tidy(neko_protocol)

add_library(neko_runtime STATIC
  core/log.cpp
  core/version.cpp
  runtime/descriptor_discovery.cpp
  runtime/generation_stream.cpp
  runtime/managed_worker.cpp
  runtime/prepare_generation.cpp
  runtime/reload_transaction.cpp
  runtime/native_session.cpp
  runtime/session.cpp)
target_link_libraries(neko_runtime PRIVATE neko_protocol neko_base_c)
target_include_directories(neko_runtime
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/generated>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_features(neko_runtime PUBLIC cxx_std_20)
nekomata_apply_warnings(neko_runtime)
nekomata_apply_sanitizers(neko_runtime)
nekomata_apply_clang_tidy(neko_runtime)

add_library(neko_publisher_lib STATIC runtime/publisher.cpp)
target_link_libraries(neko_publisher_lib PRIVATE neko_protocol neko_base_c)
nekomata_apply_warnings(neko_publisher_lib)
nekomata_apply_sanitizers(neko_publisher_lib)
nekomata_apply_clang_tidy(neko_publisher_lib)

add_subdirectory(backends)
