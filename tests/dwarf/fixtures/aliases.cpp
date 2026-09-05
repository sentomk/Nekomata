extern "C" {
int implementation() {
  return 7;
}
int alias_one() __attribute__((alias("implementation")));
int alias_two() __attribute__((weak, alias("implementation")));
}

int main() {
  return implementation() + alias_one() + alias_two() == 21 ? 0 : 1;
}
