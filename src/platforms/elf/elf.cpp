#include <neko/platforms/elf.hpp>

#include <memory>

#include "code_pages.hpp"
#include "loader.hpp"
#include "process_symbols.hpp"

namespace neko::elf {

neko::backend_bundle create_backend() {
  backend_bundle bundle;
  auto symbols = std::make_shared<process_symbols>();
  bundle.symbols = symbols;
  bundle.state = symbols; // same table backs both roles
  bundle.substituter = std::make_shared<code_pages>();
  bundle.loader = std::make_shared<loader>(*symbols, *symbols, *bundle.substituter);
  // planner: the kernel's trivial_planner is installed by reload_session.
  return bundle;
}

} // namespace neko::elf
