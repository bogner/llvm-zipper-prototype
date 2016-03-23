; REQUIRES: x86
; RUN: llvm-as %p/Inputs/linkonce-odr.ll -o %t1.o
; RUN: llc %s -o %t2.o -filetype=obj
; RUN: ld.lld %t1.o %t2.o -o %t.so -shared -save-temps
; RUN: llvm-dis %t.so.lto.opt.bc -o - | FileCheck %s

target datalayout = "e-m:e-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"
declare void @f()

define void @g() {
  call void @f()
  ret void
}

; Be sure that 'f' is kept and has weak_odr visibility.
; CHECK: define weak_odr void @f()
