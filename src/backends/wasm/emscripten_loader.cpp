#include "emscripten_loader.hpp"

#include <dlfcn.h>
#include <emscripten.h>

#include <utility>

namespace neko::wasm {
namespace {

class loaded_image final : public module_image {
public:
  explicit loaded_image(void* handle) : handle_(handle) {
    auto entry = reinterpret_cast<descriptor_function>(dlsym(handle_, descriptor_export));
    descriptor_ = entry == nullptr ? nullptr : entry();
  }
  ~loaded_image() override {
    if (!resident_) {
      // Release our loader reference. Emscripten need not reclaim linear
      // memory or table slots; this is not a module-unloading guarantee.
      dlclose(handle_);
    }
  }
  const module_header* descriptor() const noexcept override { return descriptor_; }
  void keep_resident() noexcept override { resident_ = true; }

private:
  void* handle_;
  const module_header* descriptor_ = nullptr;
  bool resident_ = false;
};

struct load_request {
  std::string path;
  module_loader::completion complete;
};

void loaded(void* context, void* handle) {
  const std::unique_ptr<load_request> request{static_cast<load_request*>(context)};
  request->complete({std::make_unique<loaded_image>(handle), {}});
}

void failed(void* context) {
  const std::unique_ptr<load_request> request{static_cast<load_request*>(context)};
  const char* diagnostic = dlerror();
  request->complete({nullptr, diagnostic == nullptr ? "wasm module load failed" : diagnostic});
}

} // namespace

void emscripten_loader::open(std::string path, completion complete) {
  auto request = std::make_unique<load_request>(load_request{std::move(path), std::move(complete)});
  auto* context = request.release();
  // The callback may run synchronously for a cached module. Do not access
  // context after this call; it belongs to the callback, not the loader.
  emscripten_dlopen(context->path.c_str(), RTLD_NOW | RTLD_LOCAL, context, loaded, failed);
}

} // namespace neko::wasm
