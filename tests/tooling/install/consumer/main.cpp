// The installed-package consumer: everything arrives through find_package
// and the neko:: headers, with no source-tree include paths anywhere.
#include <neko/neko.hpp>

#include <cstdio>

int main() {
  const auto version = neko::version_string();
  if (version.empty()) {
    return 1;
  }
  std::puts("consumer linked and ran");
  return 0;
}
