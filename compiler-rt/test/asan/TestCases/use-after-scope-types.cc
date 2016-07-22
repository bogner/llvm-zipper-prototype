// RUN: %clangxx_asan -std=c++11 -O0 -fsanitize-address-use-after-scope %s -o %t
// RUN: export %env_asan_opts=detect_stack_use_after_scope=1
// RUN: not %run %t 0 2>&1 | FileCheck %s
// RUN: not %run %t 1 2>&1 | FileCheck %s
// RUN: not %run %t 2 2>&1 | FileCheck %s
// RUN: not %run %t 3 2>&1 | FileCheck %s
// RUN: not %run %t 4 2>&1 | FileCheck %s
// RUN: not %run %t 5 2>&1 | FileCheck %s
// RUN: not %run %t 6 2>&1 | FileCheck %s
// RUN: not %run %t 7 2>&1 | FileCheck %s
// RUN: not %run %t 8 2>&1 | FileCheck %s
// RUN: not %run %t 9 2>&1 | FileCheck %s
// RUN: not %run %t 10 2>&1 | FileCheck %s
// RUN: not %run %t 11 2>&1 | FileCheck %s

// RUN: %env_asan_opts=detect_stack_use_after_scope=0 %run %t 12

#include <stdlib.h>
#include <string>
#include <vector>

template <class T> struct Ptr {
  void Store(T *ptr) { t = ptr; }

  void Access() { *t = {}; }

  T *t;
};

template <class T, size_t N> struct Ptr<T[N]> {
  using Type = T[N];
  void Store(Type *ptr) { t = *ptr; }

  void Access() { *t = {}; }

  T *t;
};

template <class T> void test() {
  Ptr<T> ptr;
  {
    T x;
    ptr.Store(&x);
  }

  ptr.Access();
  // CHECK: ERROR: AddressSanitizer: stack-use-after-scope
  // CHECK:  #{{[0-9]+}} 0x{{.*}} in void test{{.*}}(){{.*}}use-after-scope-types.cc:[[@LINE-2]]
  // CHECK: Address 0x{{.*}} is located in stack of thread T{{.*}} at offset [[OFFSET:[^ ]+]] in frame
  // {{\[}}[[OFFSET]], {{[0-9]+}}) 'x'
}

int main(int argc, char **argv) {
  using Tests = void (*)();
  Tests tests[] = {
    &test<bool>,
    &test<char>,
    &test<int>,
    &test<double>,
    &test<float>,
    &test<uint64_t>,
    &test<void*>,
    &test<std::vector<std::string>>,
    &test<int[3]>,
    &test<int[1000]>,
    &test<char[3]>,
    &test<char[1000]>,
  };

  int n = atoi(argv[1]);
  if (n == sizeof(tests) / sizeof(tests[0])) {
    for (auto te : tests)
      te();
  } else {
    tests[n]();
  }

  return 0;
}
