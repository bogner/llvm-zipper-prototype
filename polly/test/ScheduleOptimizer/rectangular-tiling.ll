; RUN: opt %loadPolly -polly-detect-unprofitable -polly-opt-isl -analyze -polly-ast -polly-tile-sizes=256,16 < %s | FileCheck %s
; RUN: opt %loadPolly -polly-detect-unprofitable -polly-opt-isl -analyze -polly-tiling=false -polly-ast -polly-tile-sizes=256,16 -polly-no-early-exit < %s | FileCheck %s --check-prefix=NOTILING

; RUN: opt %loadPolly -polly-detect-unprofitable -polly-opt-isl -analyze \
; RUN:                -polly-2nd-level-tiling -polly-ast \
; RUN:                -polly-tile-sizes=256,16 -polly-no-early-exit \
; RUN:                -polly-2nd-level-tile-sizes=16,8 < %s | \
; RUN: FileCheck %s --check-prefix=TWOLEVEL

; CHECK: for (int c0 = 0; c0 <= 3; c0 += 1)
; CHECK:   for (int c1 = 0; c1 <= 31; c1 += 1)
; CHECK:     for (int c2 = 0; c2 <= 255; c2 += 1)
; CHECK:       for (int c3 = 0; c3 <= 15; c3 += 1)
; CHECK:         Stmt_for_body3(256 * c0 + c2, 16 * c1 + c3);

; NOTILING: for (int c0 = 0; c0 <= 1023; c0 += 1)
; NOTILING:   for (int c1 = 0; c1 <= 511; c1 += 1)
; NOTILING:     Stmt_for_body3(c0, c1);


; TWOLEVEL: for (int c0 = 0; c0 <= 3; c0 += 1)
; TWOLEVEL:   for (int c1 = 0; c1 <= 31; c1 += 1)
; TWOLEVEL:     for (int c2 = 0; c2 <= 15; c2 += 1)
; TWOLEVEL:       for (int c3 = 0; c3 <= 1; c3 += 1)
; TWOLEVEL:         for (int c4 = 0; c4 <= 15; c4 += 1)
; TWOLEVEL:           for (int c5 = 0; c5 <= 7; c5 += 1)
; TWOLEVEL:             Stmt_for_body3(256 * c0 + 16 * c2 + c4, 16 * c1 + 8 * c3 + c5);


target datalayout = "e-m:e-p:32:32-i64:64-v128:64:128-n32-S64"

; Function Attrs: nounwind
define void @rect([512 x i32]* %A) {
entry:
  br label %entry.split

entry.split:                                      ; preds = %entry
  br label %for.body3.lr.ph

for.body3.lr.ph:                                  ; preds = %for.inc5, %entry.split
  %i.0 = phi i32 [ 0, %entry.split ], [ %inc6, %for.inc5 ]
  br label %for.body3

for.body3:                                        ; preds = %for.body3.lr.ph, %for.body3
  %j.0 = phi i32 [ 0, %for.body3.lr.ph ], [ %inc, %for.body3 ]
  %mul = mul nsw i32 %j.0, %i.0
  %rem = srem i32 %mul, 42
  %arrayidx4 = getelementptr inbounds [512 x i32], [512 x i32]* %A, i32 %i.0, i32 %j.0
  store i32 %rem, i32* %arrayidx4, align 4
  %inc = add nsw i32 %j.0, 1
  %cmp2 = icmp slt i32 %inc, 512
  br i1 %cmp2, label %for.body3, label %for.inc5

for.inc5:                                         ; preds = %for.body3
  %inc6 = add nsw i32 %i.0, 1
  %cmp = icmp slt i32 %inc6, 1024
  br i1 %cmp, label %for.body3.lr.ph, label %for.end7

for.end7:                                         ; preds = %for.inc5
  ret void
}
