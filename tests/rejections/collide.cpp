// collide.cpp — a second translation unit with a file-static function whose
// name collides with the one the disambiguation case introduces into the hot
// translation unit.
//
// `helper` is static, so the two definitions are legal and the linker keeps
// both symbols under the same name. The symbol table alone cannot say which is
// which; that is the ambiguity the manifest resolves. This unit prints its own
// marker so a wrong choice is visible: if the reload redirected *this* helper,
// the marker would change.

#include <cstdio>

static void helper() {
  std::printf("[collide] helper\n");
}

// The C-linkage function and explicit object-file label below give the process
// symbol table unique, file-local definitions whose spellings can also appear
// as undefined globals in a fresh object. The runtime linker must not treat
// name equality as permission to cross that binding boundary.
extern "C" {
static volatile int private_host_state asm("private_host_state") = 23;

static int private_host_value() {
  return 17;
}
}

void use_collide_helper() {
  helper();
  (void)private_host_state;
  (void)private_host_value();
}
