# REQUIRES: x86
# RUN: llvm-mc -filetype=obj -triple=x86_64-unknown-linux %s -o %t

## Check that padding value works:
# RUN: echo "SECTIONS { .mysec : { *(.mysec*) } =0x112233445566778899 }" > %t.script
# RUN: ld.lld -o %t.out --script %t.script %t
# RUN: hexdump -C %t.out | FileCheck -check-prefix=YES %s
# YES: 00000120  66 22 33 44 55 66 77 88 99 11 22 33 44 55 66 77

## Confirming that address was correct:
# RUN: echo "SECTIONS { .mysec : { *(.mysec*) } =0x998877665544332211 }" > %t.script
# RUN: ld.lld -o %t.out --script %t.script %t
# RUN: hexdump -C %t.out | FileCheck -check-prefix=YES2 %s
# YES2: 00000120  66 88 77 66 55 44 33 22 11 99 88 77 66 55 44

## Default padding value is 0x00:
# RUN: echo "SECTIONS { .mysec : { *(.mysec*) } }" > %t.script
# RUN: ld.lld -o %t.out --script %t.script %t
# RUN: hexdump -C %t.out | FileCheck -check-prefix=NO %s
# NO: 00000120  66 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00

## Decimal value.
# RUN: echo "SECTIONS { .mysec : { *(.mysec*) } =99 }" > %t.script
# RUN: ld.lld -o %t.out --script %t.script %t
# RUN: hexdump -C %t.out | FileCheck -check-prefix=DEC %s
# DEC: 00000120  66 63 63 63 63 63 63 63  63 63 63 63 63 63 63 63 

## Invalid hex value:
# RUN: echo "SECTIONS { .mysec : { *(.mysec*) } =0x99XX }" > %t.script
# RUN: not ld.lld -o %t.out --script %t.script %t 2>&1 \
# RUN:   | FileCheck --check-prefix=ERR2 %s
# ERR2: not a hexadecimal value: XX

.section        .mysec.1,"a"
.align  16
.byte   0x66

.section        .mysec.2,"a"
.align  16
.byte   0x66

.globl _start
_start:
 nop
