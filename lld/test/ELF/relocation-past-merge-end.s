// REQUIRES: x86
// RUN: llvm-mc %s -o %t.o -filetype=obj -triple=x86_64-pc-linux
// RUN: not ld.lld %t.o -o %t.so -shared 2>&1 | FileCheck %s
// CHECK: entry is past the end of the section

        .long .foo + 1
        .section	.foo,"aM",@progbits,4
