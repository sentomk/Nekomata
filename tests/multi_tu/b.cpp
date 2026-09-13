int b_value() {
  return 1;
}

__attribute__((naked)) void b_guard() {
  __asm__ volatile("nop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tret");
}
