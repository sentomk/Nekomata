#pragma once

#include "candidate.hpp"
#include "manifest_fetcher.hpp"

namespace neko::wasm {

// Browser event-loop adapter. Paths must identify immutable module contents.
// The adapter fetches the artifact bytes, verifies the contract's SHA-256
// before instantiation, stages the bytes under a private MEMFS path, and
// owns the loader reference. Completion is always asynchronous.
class emscripten_loader final : public module_loader {
public:
  void open(std::string path, std::string_view sha256, completion complete) override;
};

// Fetches one manifest URL as text on the same event loop. Completion is
// always asynchronous.
class emscripten_manifest_fetcher final : public manifest_fetcher {
public:
  void fetch(std::string url, completion complete) override;
};

} // namespace neko::wasm
