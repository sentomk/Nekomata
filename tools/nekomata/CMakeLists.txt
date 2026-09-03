add_executable(nekomata main.cpp)
target_link_libraries(nekomata PRIVATE nekomata::hlr)
nekomata_apply_warnings(nekomata)
nekomata_apply_sanitizers(nekomata)
