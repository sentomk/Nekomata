cmake_minimum_required(VERSION 3.21)
project(neko_install_consumer LANGUAGES CXX)

find_package(nekomata CONFIG REQUIRED)
message(STATUS "consumer found nekomata at: ${nekomata_DIR}")

if(NOT TARGET nekomata::runtime)
  message(FATAL_ERROR "the installed package does not define nekomata::runtime")
endif()
get_target_property(neko_consumer_includes nekomata::runtime INTERFACE_INCLUDE_DIRECTORIES)
if(NOT neko_consumer_includes)
  message(FATAL_ERROR
    "nekomata::runtime carries no include directories; the install interface is broken")
endif()

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE nekomata::runtime)
