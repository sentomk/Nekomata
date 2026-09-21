#include "emscripten_loader.hpp"

#include <base/c/neko_sha256.h>
#include <dlfcn.h>
#include <emscripten/fetch.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
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
  std::string url;
  std::string sha256;
  module_loader::completion complete;
};

void deliver(load_request& request, module_load_result result) {
  auto complete = std::move(request.complete);
  complete(std::move(result));
}

void hex_encode(const std::uint8_t digest[32], char out[65]) {
  static constexpr char digits[] = "0123456789abcdef";
  for (std::size_t index = 0; index < 32; ++index) {
    out[2 * index] = digits[digest[index] >> 4];
    out[2 * index + 1] = digits[digest[index] & 0xf];
  }
  out[64] = '\0';
}

// Stage the verified bytes under a private path so no two generations share
// a dlopen identity. Staged files stay resident with their module code.
std::string stage_bytes(const std::uint8_t* data, std::size_t size) {
  static std::atomic<std::uint32_t> counter{0};
  const std::string path = "/neko-artifact-" + std::to_string(counter.fetch_add(1)) + ".wasm";
  std::FILE* file = std::fopen(path.c_str(), "wb");
  if (file == nullptr) {
    return {};
  }
  const std::size_t written = std::fwrite(data, 1, size, file);
  std::fclose(file);
  return written == size ? path : std::string{};
}

void fetched(emscripten_fetch_t* fetch) {
  const std::unique_ptr<load_request> request{static_cast<load_request*>(fetch->userData)};

  neko_sha256_context context;
  std::uint8_t digest[32];
  neko_sha256_init(&context);
  neko_sha256_update(&context, fetch->data, fetch->numBytes);
  neko_sha256_final(&context, digest);
  char hex[65];
  hex_encode(digest, hex);
  if (request->sha256 != hex) {
    deliver(*request, {nullptr,
                       "wasm artifact digest mismatch for '" + request->url + "': expected " +
                           request->sha256 + ", fetched " + hex,
                       module_load_status::digest_mismatch});
    return;
  }

  const std::string staged = stage_bytes(reinterpret_cast<const std::uint8_t*>(fetch->data),
                                         static_cast<std::size_t>(fetch->numBytes));
  if (staged.empty()) {
    deliver(*request,
            {nullptr, "cannot stage wasm artifact bytes", module_load_status::load_failed});
    return;
  }

  void* handle = dlopen(staged.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (handle == nullptr) {
    const char* diagnostic = dlerror();
    deliver(*request,
            {nullptr, diagnostic == nullptr ? "wasm module instantiation failed" : diagnostic,
             module_load_status::load_failed});
    return;
  }
  deliver(*request, {std::make_unique<loaded_image>(handle), {}});
}

void fetch_failed(emscripten_fetch_t* fetch) {
  const std::unique_ptr<load_request> request{static_cast<load_request*>(fetch->userData)};
  deliver(*request, {nullptr, "wasm artifact fetch failed for '" + request->url + "'",
                     module_load_status::load_failed});
}

} // namespace

// Bytes-first pipeline: fetch the artifact, verify its SHA-256 before
// instantiation, stage the bytes under a private MEMFS path, and dlopen
// there. Verification is the only gate between the network and the
// compiler; an unverified artifact never reaches instantiation.
void emscripten_loader::open(std::string path, std::string_view sha256, completion complete) {
  auto request = std::make_unique<load_request>();
  request->url = std::move(path);
  request->sha256 = std::string{sha256};
  request->complete = std::move(complete);
  const std::string url = request->url;

  emscripten_fetch_attr_t attr;
  emscripten_fetch_attr_init(&attr);
  std::strcpy(attr.requestMethod, "GET");
  attr.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY;
  attr.userData = request.release();
  attr.onsuccess = fetched;
  attr.onerror = fetch_failed;
  emscripten_fetch(&attr, url.c_str());
}

} // namespace neko::wasm
