#include "neko/backends/elf.hpp"
#include "neko/runtime/session.hpp"

int main() {
  neko::reload_session session{neko::elf::create_backend()};
  session.watch("hot.new.o");

  while
}
