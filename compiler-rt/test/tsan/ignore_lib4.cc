// RUN: %clangxx_tsan -O1 %s -DLIB -fPIC -shared -o %T/libignore_lib4.so
// RUN: %clangxx_tsan -O1 %s -o %t
// RUN: %env_tsan_opts=suppressions='%s.supp' %run %t 2>&1 | FileCheck %s

// Longjmp assembly has not been implemented for mips64 yet
// XFAIL: mips64

// Test longjmp in ignored lib.
// It used to crash since we jumped out of ScopedInterceptor scope.

#include "test.h"
#include <setjmp.h>
#include <string.h>
#include <errno.h>
#include <libgen.h>
#include <string>

#ifdef LIB

extern "C" void myfunc() {
  for (int i = 0; i < (1 << 20); i++) {
    jmp_buf env;
    if (!setjmp(env))
      longjmp(env, 1);
  }
}

#else

int main(int argc, char **argv) {
  std::string lib = std::string(dirname(argv[0])) + "/libignore_lib4.so";
  void *h = dlopen(lib.c_str(), RTLD_GLOBAL | RTLD_NOW);
  void (*func)() = (void(*)())dlsym(h, "myfunc");
  func();
  fprintf(stderr, "DONE\n");
  return 0;
}

#endif

// CHECK: DONE
