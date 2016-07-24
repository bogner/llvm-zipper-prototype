# REQUIRES: x86
# RUN: llvm-mc -filetype=obj -triple=x86_64-unknown-linux %s -o %t

# RUN: echo "SECTIONS { \
# RUN:  . = 0x10000; \
# RUN:  .aaa : \
# RUN:  { \
# RUN:   *(.aaa) \
# RUN:  } \
# RUN:  . = ALIGN(4096); \
# RUN:  .bbb : \
# RUN:  { \
# RUN:   *(.bbb) \
# RUN:  } \
# RUN:  . = ALIGN(4096 * 4); \
# RUN:  .ccc : \
# RUN:  { \
# RUN:   *(.ccc) \
# RUN:  } \
# RUN: }" > %t.script
# RUN: ld.lld -o %t1 --script %t.script %t
# RUN: llvm-objdump -section-headers %t1 | FileCheck %s
# CHECK:      Sections:
# CHECK-NEXT: Idx Name          Size      Address          Type
# CHECK-NEXT:   0               00000000 0000000000000000
# CHECK-NEXT:   1 .aaa          00000008 0000000000010000 DATA
# CHECK-NEXT:   2 .bbb          00000008 0000000000011000 DATA
# CHECK-NEXT:   3 .ccc          00000008 0000000000014000 DATA

.global _start
_start:
 nop

.section .aaa, "a"
.quad 0

.section .bbb, "a"
.quad 0

.section .ccc, "a"
.quad 0
