cmake_minimum_required(VERSION 3.21)
project(neko_install_consumer LANGUAGES CXX)

find_package(nekomata CONFIG REQUIRED)

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE nekomata::runtime)
