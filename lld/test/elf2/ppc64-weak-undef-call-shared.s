# RUN: llvm-mc -filetype=obj -triple=powerpc64-unknown-linux %s -o %t.o
# RUN: ld.lld2 -shared %t.o -o %t.so
# RUN: llvm-readobj -t -r -dyn-symbols %t.so | FileCheck %s
# REQUIRES: ppc

.section        ".toc","aw"
.quad weakfunc
// CHECK-NOT: R_PPC64_RELATIVE

.weak weakfunc

