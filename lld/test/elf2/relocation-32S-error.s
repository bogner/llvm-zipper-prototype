// RUN: llvm-mc -filetype=obj -triple=x86_64-pc-linux %s -o %t
// RUN: not lld -flavor gnu2 %t -o %t2 2>&1 | FileCheck %s
// REQUIRES: x86

.global _start
_start:
  movq _start - 0x1000000000000, %rdx

#CHECK: R_X86_64_32S out of range
