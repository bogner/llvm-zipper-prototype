; REQUIRES: x86
; RUN: rm -f a.out a.out.lto.bc a.out.lto.o
; RUN: llvm-as %s -o %t.o
; RUN: llvm-as %p/Inputs/save-temps.ll -o %t2.o
; RUN: ld.lld -shared -m elf_x86_64 %t.o %t2.o -save-temps
; RUN: llvm-nm a.out | FileCheck %s
; RUN: llvm-nm a.out.lto.bc | FileCheck %s
; RUN: llvm-nm a.out.lto.o | FileCheck %s
; RUN: llvm-dis a.out.lto.bc

target triple = "x86_64-unknown-linux-gnu"
target datalayout = "e-m:e-i64:64-f80:128-n8:16:32:64-S128"

define void @foo() {
  ret void
}

; CHECK: T bar
; CHECK: T foo
