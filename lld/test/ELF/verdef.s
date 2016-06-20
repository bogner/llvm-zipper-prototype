# REQUIRES: x86
# RUN: llvm-mc -filetype=obj -triple=x86_64-unknown-linux %s -o %t.o
# RUN: echo "LIBSAMPLE_1.0{  \
# RUN:          global: a;   \
# RUN:          local: *; }; \
# RUN:       LIBSAMPLE_2.0{  \
# RUN:          global: b;   \
# RUN:          local: *; }; \
# RUN:       LIBSAMPLE_3.0{  \
# RUN:          global: c;   \
# RUN:          local: *; };" > %t.script
# RUN: ld.lld --version-script %t.script -shared -soname shared %t.o -o %t.so
# RUN: llvm-readobj -V -dyn-symbols %t.so | FileCheck --check-prefix=DSO %s

# DSO:        Version symbols {
# DSO-NEXT:   Section Name: .gnu.version
# DSO-NEXT:   Address: 0x228
# DSO-NEXT:   Offset: 0x228
# DSO-NEXT:   Link: 1
# DSO-NEXT:   Symbols [
# DSO-NEXT:     Symbol {
# DSO-NEXT:       Version: 0
# DSO-NEXT:       Name: @
# DSO-NEXT:     }
# DSO-NEXT:     Symbol {
# DSO-NEXT:       Version: 2
# DSO-NEXT:       Name: a@@LIBSAMPLE_1.0
# DSO-NEXT:     }
# DSO-NEXT:     Symbol {
# DSO-NEXT:       Version: 3
# DSO-NEXT:       Name: b@@LIBSAMPLE_2.0
# DSO-NEXT:     }
# DSO-NEXT:     Symbol {
# DSO-NEXT:       Version: 4
# DSO-NEXT:       Name: c@@LIBSAMPLE_3.0
# DSO-NEXT:     }
# DSO-NEXT:   ]
# DSO-NEXT: }
# DSO-NEXT: Version definition {
# DSO-NEXT:   Section Name: .gnu.version_d
# DSO-NEXT:   Address: 0x230
# DSO-NEXT:   Offset: 0x230
# DSO-NEXT:   Link: 5
# DSO-NEXT:   Entries [
# DSO-NEXT:     Entry {
# DSO-NEXT:       Offset: 0x0
# DSO-NEXT:       Rev: 1
# DSO-NEXT:       Flags: 1
# DSO-NEXT:       Index: 1
# DSO-NEXT:       Cnt: 1
# DSO-NEXT:       Hash: 127830196
# DSO-NEXT:       Name:
# DSO-NEXT:     }
# DSO-NEXT:     Entry {
# DSO-NEXT:       Offset: 0x14
# DSO-NEXT:       Rev: 1
# DSO-NEXT:       Flags: 0
# DSO-NEXT:       Index: 2
# DSO-NEXT:       Cnt: 1
# DSO-NEXT:       Hash: 98457184
# DSO-NEXT:       Name: LIBSAMPLE_1.0
# DSO-NEXT:     }
# DSO-NEXT:     Entry {
# DSO-NEXT:       Offset: 0x28
# DSO-NEXT:       Rev: 1
# DSO-NEXT:       Flags: 0
# DSO-NEXT:       Index: 3
# DSO-NEXT:       Cnt: 1
# DSO-NEXT:       Hash: 98456416
# DSO-NEXT:       Name: LIBSAMPLE_2.0
# DSO-NEXT:     }
# DSO-NEXT:     Entry {
# DSO-NEXT:       Offset: 0x3C
# DSO-NEXT:       Rev: 1
# DSO-NEXT:       Flags: 0
# DSO-NEXT:       Index: 4
# DSO-NEXT:       Cnt: 1
# DSO-NEXT:       Hash: 98456672
# DSO-NEXT:       Name: LIBSAMPLE_3.0
# DSO-NEXT:     }
# DSO-NEXT:   ]
# DSO-NEXT: }
# DSO-NEXT: SHT_GNU_verneed {
# DSO-NEXT: }

## Check that we can link agains DSO we produced.
# RUN: llvm-mc -filetype=obj -triple=x86_64-unknown-linux %S/Inputs/verdef.s -o %tmain.o
# RUN: ld.lld %tmain.o %t.so -o %tout
# RUN: llvm-readobj -V %tout | FileCheck --check-prefix=MAIN %s

# MAIN:      Version symbols {
# MAIN-NEXT:   Section Name: .gnu.version
# MAIN-NEXT:   Address: 0x10228
# MAIN-NEXT:   Offset: 0x228
# MAIN-NEXT:   Link: 1
# MAIN-NEXT:   Symbols [
# MAIN-NEXT:     Symbol {
# MAIN-NEXT:       Version: 0
# MAIN-NEXT:       Name: @
# MAIN-NEXT:     }
# MAIN-NEXT:     Symbol {
# MAIN-NEXT:       Version: 2
# MAIN-NEXT:       Name: a@LIBSAMPLE_1.0
# MAIN-NEXT:     }
# MAIN-NEXT:     Symbol {
# MAIN-NEXT:       Version: 3
# MAIN-NEXT:       Name: b@LIBSAMPLE_2.0
# MAIN-NEXT:     }
# MAIN-NEXT:     Symbol {
# MAIN-NEXT:       Version: 4
# MAIN-NEXT:       Name: c@LIBSAMPLE_3.0
# MAIN-NEXT:     }
# MAIN-NEXT:   ]
# MAIN-NEXT: }
# MAIN-NEXT: Version definition {
# MAIN-NEXT: }

.globl a
.type  a,@function
a:
retq

.globl b
.type  b,@function
b:
retq

.globl c
.type  c,@function
c:
retq
