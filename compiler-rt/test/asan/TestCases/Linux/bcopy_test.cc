// RUN: %clangxx_asan  %s -o %t
// RUN: not %run %t   2>&1 | FileCheck %s --check-prefix=A1
// RUN: not %run %t 1 2>&1 | FileCheck %s --check-prefix=A2
// RUN: %env_asan_opts=replace_intrin=0 %run %t

#include <strings.h>
int main(int argc, char **argv) {
  char a1[] = {1, 2, 3, 4, 5, 6, 7, 8};
  char a2[] = {3, 4, 5, 6, 7, 8, 9};
  if (argc == 1)
    bcopy(a1, a2, sizeof(a1));  // BOOM
  else
    bcopy(a2, a1, sizeof(a1));  // BOOM
  // A1: AddressSanitizer: stack-buffer-overflow
  // A1: {{#0.*memmove}}
  // A1: 'a2' <== Memory access at offset
  //
  // A2: AddressSanitizer: stack-buffer-overflow
  // A2: {{#0.*memmove}}
  // A2: 'a2' <== Memory access at offset
  return 0;
}
