# Candidate ownership, descriptor validation, offer delivery, and the
# browser reload session are testable without a browser.
add_library(neko_wasm_candidate STATIC candidate.cpp offer_poller.cpp session.cpp)
target_include_directories(neko_wasm_candidate PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(neko_wasm_candidate PUBLIC neko_protocol)
nekomata_apply_warnings(neko_wasm_candidate)
nekomata_apply_sanitizers(neko_wasm_candidate)
nekomata_apply_clang_tidy(neko_wasm_candidate)
if(EMSCRIPTEN)
  target_sources(neko_wasm_candidate PRIVATE emscripten_loader.cpp)
endif()
