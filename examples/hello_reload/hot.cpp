// hot.cpp — the translation unit nekomata swaps while the process runs.
//
// Edit this file while the demo runs; the next tick() call picks up the new
// logic, and g_counter / s_calls keep their values (state preservation).
// run_demo.sh edits the marked lines to produce "v2".

#include <cstdio>

int g_counter = 0;      // mutable global: state must survive reloads
static int s_calls = 0; // file-static: same, via local-symbol matching

void tick() {
  ++g_counter; // v1: step by 1  (v2 edits this line)
  ++s_calls;
  std::printf("[v1] tick #%d, g_counter=%d\n", s_calls, g_counter); // v2 edits this line
  std::fflush(stdout); // regression guard: undefined DATA symbol (copy-relocated
                       // `stdout`) must resolve via the exe symtab, not a call
                       // trampoline — else fflush reads trampoline bytes (SIGSEGV)
}
