// nekomata CLI.
//
// Phase 1 scope: a status/version stub. The reload-driving CLI
// (attach to process, watch sources, trigger reloads) lands together with
// the ELF backend.

#include <neko/version.hpp>

#include <cstdio>
#include <cstring>
#include <string>

namespace {

void printUsage(std::FILE* out) {
    std::fputs("nekomata — native hot-reload for C/C++\n\n"
               "Usage: nekomata [options]\n\n"
               "Options:\n"
               "  --version  Print version and exit\n"
               "  --help     Print this help and exit\n",
               out);
}

} // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--version") == 0) {
            std::printf("nekomata %s\n", neko::version_string().c_str());
            return 0;
        }
        if (std::strcmp(argv[i], "--help") == 0) {
            printUsage(stdout);
            return 0;
        }
        std::fprintf(stderr, "nekomata: unknown option '%s'\n\n", argv[i]);
        printUsage(stderr);
        return 2;
    }

    std::printf("nekomata %s — hot reload core is not implemented yet "
                "(phase 1 in progress).\n",
                neko::version_string().c_str());
    return 0;
}
