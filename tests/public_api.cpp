// Exercise the common public entry points without the umbrella or private headers.
#include <neko/fwd.hpp>
#include <neko/log.hpp>
#include <neko/session.hpp>
#include <neko/version.hpp>

#include <stdexcept>
#include <string_view>

int main() {
  const auto version = neko::version_string();
  if (version != NEKOMATA_VERSION_STRING) {
    return 1;
  }

  const std::string_view tag = neko::log_tag(neko::log_level::ok);
  if (tag != "(=^ω^=)" && tag != "\033[32m(=^ω^=)\033[0m") {
    return 2;
  }

  // The public session header must support construction/destruction even
  // though it only forward-declares the backend interface classes.
  try {
    neko::reload_session session{neko::backend_bundle{}};
    return 3;
  } catch (const std::runtime_error&) {
    // Missing backend components are intentionally rejected.
  }

  neko::log(neko::log_level::ok, "public API linked: %s (%d)\n", version.c_str(), 42);
  return 0;
}
