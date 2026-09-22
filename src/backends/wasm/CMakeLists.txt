# Candidate ownership, descriptor validation, offer delivery, and the
# browser reload session are testable without a browser.
add_library(neko_wasm_candidate STATIC candidate.cpp entry_set.cpp registration.cpp offer_poller.cpp session.cpp "${PROJECT_SOURCE_DIR}/src/core/log.cpp")
target_include_directories(neko_wasm_candidate
  PRIVATE "${PROJECT_SOURCE_DIR}/src"
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(neko_wasm_candidate PUBLIC neko_protocol)
target_compile_features(neko_wasm_candidate PUBLIC cxx_std_20)
nekomata_apply_warnings(neko_wasm_candidate)
nekomata_apply_sanitizers(neko_wasm_candidate)
nekomata_apply_clang_tidy(neko_wasm_candidate)
if(EMSCRIPTEN)
  target_sources(neko_wasm_candidate PRIVATE wasm.cpp emscripten_loader.cpp emscripten_scheduler.cpp)
  target_link_libraries(neko_wasm_candidate PRIVATE neko_runtime neko_base_c)
  set_target_properties(neko_wasm_candidate neko_runtime neko_protocol neko_base_c
    PROPERTIES POSITION_INDEPENDENT_CODE ON)
  target_compile_options(neko_wasm_candidate PUBLIC -fexceptions -fPIC)
  target_compile_options(neko_runtime PRIVATE -fexceptions)
  target_compile_options(neko_protocol PRIVATE -fexceptions)
  target_link_options(neko_wasm_candidate INTERFACE
    -sMAIN_MODULE=1 -sFETCH=1 -sALLOW_MEMORY_GROWTH=1 -sDISABLE_EXCEPTION_CATCHING=0)
endif()
