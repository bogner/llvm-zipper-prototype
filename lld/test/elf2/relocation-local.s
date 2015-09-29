// Test that relocation of local symbols is working.
// RUN: llvm-mc -filetype=obj -triple=x86_64-pc-linux %s -o %t
// RUN: lld -flavor gnu2 %t -o %t2
// RUN: llvm-objdump -s -d %t2 | FileCheck %s
// REQUIRES: x86


.global _start
_start:
  call lulz

.zero 4
lulz:

.section       .text2,"ax",@progbits
R_X86_64_32:
  movl $R_X86_64_32, %edx

// FIXME: this would be far more self evident if llvm-objdump printed
// constants in hex.
// CHECK: Disassembly of section .text2:
// CHECK-NEXT: R_X86_64_32:
// CHECK-NEXT:  1100c: {{.*}} movl $69644, %edx

.section .R_X86_64_32S,"ax",@progbits
R_X86_64_32S:
  movq lulz - 0x100000, %rdx

// CHECK: Disassembly of section .R_X86_64_32S:
// CHECK-NEXT: R_X86_64_32S:
// CHECK-NEXT:  {{.*}}: {{.*}} movq -978935, %rdx

.section .R_X86_64_64,"a",@progbits
R_X86_64_64:
 .quad R_X86_64_64

// CHECK:      Contents of section .R_X86_64_64:
// CHECK-NEXT:   12000 00200100 00000000
