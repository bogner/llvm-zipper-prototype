// Test string s1 overflow in strpbrk function
// RUN: %clang_asan %s -o %t && ASAN_OPTIONS=strict_string_checks=true not %run %t 2>&1 | FileCheck %s

// Test intercept_strpbrk asan option
// RUN: ASAN_OPTIONS=intercept_strpbrk=false %run %t 2>&1

#include <assert.h>
#include <string.h>

int main(int argc, char **argv) {
  char *r;
  char s2[] = "ab";
  char s1[] = {'c', 'd'};
  char s3[] = "a";
  r = strpbrk(s1, s2);
  // CHECK:'s{{[1|3]}}' <== Memory access at offset {{[0-9]+ .*}}flows this variable
  assert(r <= s3);
  return 0;
}
