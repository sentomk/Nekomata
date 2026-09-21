#pragma once

#include "candidate.hpp"

namespace neko::wasm {

// Browser event-loop adapter. Paths must identify immutable module contents.
// The adapter fetches the artifact bytes, verifies the contract's SHA-256
// before instantiation, stages the bytes under a private MEMFS path, and
// owns the loader reference. Completion is always asynchronous.
class emscripten_loader final : public module_loader {
public:
  void open(std::string path, std::string_view sha256, completion complete) override;
};

} // namespace neko::wasm
