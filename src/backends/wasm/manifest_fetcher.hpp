#pragma once

#include <functional>
#include <string>

namespace neko::wasm {

// Delivery of one manifest as text. Exactly one completion on the calling
// event loop; a completion may also run before fetch() returns.
struct manifest_text {
  bool ok = false;
  std::string text;
  std::string message;
};

// Transport seam behind the offer poller. Implementations own the URL and
// completion until delivering the result; they never parse the text.
class manifest_fetcher {
public:
  using completion = std::function<void(manifest_text)>;
  virtual ~manifest_fetcher() = default;
  virtual void fetch(std::string url, completion complete) = 0;
};

} // namespace neko::wasm
