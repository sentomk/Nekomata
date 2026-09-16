// Fixture: a translation unit whose only external reference is a call to an
// undefined function. Loading it forces the ELF backend to emit an in-arena
// PLT trampoline for that call, which is what plt_trampoline.cpp inspects.
// The probe target is defined by the test binary, not here, so the symbol
// stays SHN_UNDEF in this object. The call takes no arguments on purpose:
// passing a string literal would add an absolute 32-bit .rodata reference,
// which only resolves when the host image (and thus the arena) sits in low
// memory — a separate loader boundary, irrelevant to the trampoline bytes.
extern "C" void neko_plt_probe_target();

extern "C" void neko_plt_probe_entry() {
  neko_plt_probe_target(); // R_X86_64_PLT32 against SHN_UNDEF
}
