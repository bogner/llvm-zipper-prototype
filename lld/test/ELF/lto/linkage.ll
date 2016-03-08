; REQUIRES: x86
; RUN: llvm-as %s -o %t1.o
; RUN: ld.lld -m elf_x86_64 %t1.o %t1.o -o %t.so -shared

target datalayout = "e-m:e-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

; Should not encounter a duplicate symbol error for @.str
@.str = private unnamed_addr constant [4 x i8] c"Hey\00", align 1
