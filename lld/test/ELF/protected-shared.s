// REQUIRES: x86
// RUN: llvm-mc -filetype=obj -triple=x86_64-pc-linux %s -o %t.o
// RUN: llvm-mc -filetype=obj -triple=x86_64-pc-linux %p/Inputs/protected-shared.s -o %t2.o
// RUN: ld.lld -shared %t2.o -o %t2.so
// RUN: ld.lld %t.o %t2.so -o %t
// RUN: llvm-readobj -t %t | FileCheck %s

        .global  _start
_start:
        .quad foo

        .global bar
bar:

// CHECK:      Name: bar
// CHECK-NEXT: Value:
// CHECK-NEXT: Size: 0
// CHECK-NEXT: Binding: Global
// CHECK-NEXT: Type: None
// CHECK-NEXT: Other: 0
// CHECK-NEXT: Section: .text

// CHECK:      Name: foo
// CHECK-NEXT: Value: 0x0
// CHECK-NEXT: Size: 0
// CHECK-NEXT: Binding: Global
// CHECK-NEXT: Type: None
// CHECK-NEXT: Other: 0
// CHECK-NEXT: Section: Undefined
