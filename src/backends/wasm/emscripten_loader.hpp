#pragma once

#include "candidate.hpp"

namespace neko::wasm {

// Browser event-loop adapter. Paths must identify immutable module contents;
// Emscripten can reuse an already-loaded module with the same path.
class emscripten_loader final : public module_loader {
public:
  void open(std::string path, completion complete) override;
};

} // namespace neko::wasm
