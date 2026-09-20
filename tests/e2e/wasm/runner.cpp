#include "contract.hpp"

#include <dlfcn.h>
#include <emscripten.h>

#include <cstdlib>

namespace {

const generation_descriptor* generation_a = nullptr;

EM_JS(void, report, (int ok, const char* message),
      { window.report_result({ok : !!ok, message : UTF8ToString(message)}); });

void require(bool condition, const char* message) {
  if (!condition) {
    report(0, message);
    std::abort();
  }
}

const generation_descriptor* descriptor_from(void* handle) {
  auto entry = reinterpret_cast<descriptor_fn>(dlsym(handle, "get_generation_descriptor"));
  require(entry != nullptr, "descriptor export missing");
  const auto* descriptor = entry();
  require(descriptor != nullptr, "null descriptor");
  require(descriptor->interface_version == 1, "unexpected interface version");
  return descriptor;
}

void load_failed(void*) {
  report(0, dlerror());
}

void loaded_b(void*, void* handle) {
  const auto* generation_b = descriptor_from(handle);
  require(generation_b != generation_a, "handles resolved the same descriptor");
  require(generation_b->generation_id == 2, "B descriptor identity");
  require(generation_b->identify() == 2, "B function identity");
  require(generation_a->identify() == 1, "A changed after loading B");
  report(1, "distinct handles preserve A and B exports");
}

void loaded_a(void*, void* handle) {
  generation_a = descriptor_from(handle);
  require(generation_a->generation_id == 1, "A descriptor identity");
  require(generation_a->identify() == 1, "A function identity");
  emscripten_dlopen("b.wasm", RTLD_NOW | RTLD_LOCAL, nullptr, loaded_b, load_failed);
}

} // namespace

int main() {
  emscripten_dlopen("a.wasm", RTLD_NOW | RTLD_LOCAL, nullptr, loaded_a, load_failed);
  emscripten_exit_with_live_runtime();
}
