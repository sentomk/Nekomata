# Candidate ownership and descriptor validation are testable without a browser.
add_library(neko_wasm_candidate STATIC candidate.cpp)
nekomata_apply_warnings(neko_wasm_candidate)
nekomata_apply_sanitizers(neko_wasm_candidate)
nekomata_apply_clang_tidy(neko_wasm_candidate)
