#include <neko/backend.hpp>
#include <neko/pe.hpp>

#include <memory>
#include <utility>

#include "debug/process_symbols.hpp"
#include "runtime/code_pages.hpp"
#include "runtime/loader.hpp"

namespace neko::pe {

backend_handle create_backend() {
  backend::bundle bundle;
  auto symbols = std::make_shared<process_symbols>();
  bundle.symbols = symbols;
  bundle.state = symbols; // the same DIA table backs both roles
  bundle.substituter = std::make_shared<code_pages>();
  bundle.loader = std::make_shared<loader>(*symbols, *symbols, *bundle.substituter);
  // planner: the kernel's trivial_planner is installed by reload_session.
  return backend::make_handle(std::move(bundle));
}

} // namespace neko::pe
