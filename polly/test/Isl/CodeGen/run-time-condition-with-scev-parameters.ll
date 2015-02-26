; RUN: opt %loadPolly -polly-detect-unprofitable -polly-no-early-exit -polly-codegen-isl -S -polly-delinearize < %s | FileCheck %s
; RUN: opt %loadPolly -polly-detect-unprofitable -polly-no-early-exit -polly-codegen-isl -S -polly-delinearize < %s | FileCheck %s

; CHECK: %1 = zext i32 %n to i64
; CHECK-NEST: %2 = icmp sge i64 %1, 1
; CHECK-NEST: br label %polly.split_new_and_old
; CHECK: polly.split_new_and_old:
; CHECK-NEXT: br i1 %2, label %polly.start, label %for.body4


target datalayout = "e-m:e-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

define void @init_array(i32 %n, double* %data) {
entry:
  %0 = zext i32 %n to i64
  br label %for.body4

for.body4:                                        ; preds = %for.body4, %entry
  %indvar1 = phi i64 [ %indvar.next2, %for.body4 ], [ 0, %entry ]
  %.moved.to.for.body4 = mul i64 %0, %indvar1
  %1 = add i64 %.moved.to.for.body4, 0
  %arrayidx7 = getelementptr double* %data, i64 %1
  store double undef, double* %arrayidx7, align 8
  %indvar.next2 = add i64 %indvar1, 1
  br i1 false, label %for.body4, label %for.end10

for.end10:                                        ; preds = %for.body4
  ret void
}
