#include <neko/version.hpp>

#include <cstdio>
#include <cstring>

#ifdef NEKOMATA_HAS_DWARF_INSPECTION
int inspect_binary(const char* path);
#endif

namespace {

void print_usage(std::FILE* out) {
  std::fputs("nekomata — native hot-reload for C/C++\n\n"
             "Usage: nekomata [--version | --help]\n"
             "       nekomata inspect <binary>\n\n"
             "inspect reads embedded DWARF without running or modifying the binary.\n"
             "Supported: Linux ELF64 x86-64 ET_EXEC, DWARF 4 / DWARF32, -O0.\n",
             out);
}

} // namespace

int main(int argc, char** argv) {
  if (argc == 1 || (argc == 2 && std::strcmp(argv[1], "--help") == 0)) {
    print_usage(stdout);
    return 0;
  }
  if (argc == 2 && std::strcmp(argv[1], "--version") == 0) {
    std::printf("nekomata %s\n", neko::version_string().c_str());
    return 0;
  }
  if (argc == 3 && std::strcmp(argv[1], "inspect") == 0) {
#ifdef NEKOMATA_HAS_DWARF_INSPECTION
    return inspect_binary(argv[2]);
#else
    std::fputs("inspect unavailable: enable DWARF inspection in a Linux build\n", stderr);
    return 1;
#endif
  }
  print_usage(stderr);
  return 2;
}
