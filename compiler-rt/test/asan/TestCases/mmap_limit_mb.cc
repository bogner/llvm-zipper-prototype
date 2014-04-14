// Test the mmap_limit_mb flag.
//
// RUN: %clangxx_asan -std=c++11 -O2 %s -o %t
// RUN: %t 100 16
// RUN: %t 100 1000000
// RUN: ASAN_OPTIONS=mmap_limit_mb=500 %t 100 16
// RUN: ASAN_OPTIONS=mmap_limit_mb=500 %t 100 1000000
// RUN: ASAN_OPTIONS=mmap_limit_mb=500 not %t 500 16 2>&1 | FileCheck %s
// RUN: ASAN_OPTIONS=mmap_limit_mb=500 not %t 500 1000000 2>&1 | FileCheck %s

#include <assert.h>
#include <stdlib.h>
#include <stdio.h>

#include <algorithm>
#include <vector>

int main(int argc, char **argv) {
  assert(argc == 3);
  long total_mb = atoi(argv[1]);
  long allocation_size = atoi(argv[2]);
  std::vector<char *> v;
  for (long total = total_mb << 20; total > 0; total -= allocation_size)
    v.push_back(new char[allocation_size]);
  for (auto p : v) delete[] p;
  printf("PASS\n");
  // CHECK: AddressSanitizer CHECK failed{{.*}}total_mmaped{{.*}}mmap_limit_mb
  return 0;
}
