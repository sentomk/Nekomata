cmake_minimum_required(VERSION 3.24)
project(neko_wasm_public_consumer LANGUAGES CXX)

find_package(nekomata CONFIG REQUIRED)
add_executable(public_session ../public_session.cpp)
target_link_libraries(public_session PRIVATE nekomata::neko)
target_compile_options(public_session PRIVATE -Wall -Wextra -Werror)
target_link_options(public_session PRIVATE -sENVIRONMENT=web -sASSERTIONS=2)
set_target_properties(public_session PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${FIXTURE_OUTPUT}")
