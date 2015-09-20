; RUN: opt %loadPolly -polly-detect-unprofitable -polly-scops -analyze < %s | FileCheck %s
;
; The assumed context is tricky here as the equality test for the inner loop
; allows an "unbounded" loop trip count. We assume that does not happen, thus
; if N <= 1 the outer loop is not executed and we are done, if N >= 3 the
; equality test in the inner exit condition will trigger at some point and,
; finally, if N == 2 we would have an unbounded inner loop.
;
; CHECK:      Assumed Context:
; CHECK-NEXT:   [N] -> {  : N >= 3 or N <= 1 }
;
;    int jd(int *restrict A, int x, int N) {
;      for (int i = 1; i < N; i++)
;        for (int j = 3; j < N; j++)
;          x += A[i];
;      return x;
;    }
;
target datalayout = "e-m:e-i64:64-f80:128-n8:16:32:64-S128"

define i32 @jd(i32* noalias %A, i32 %x, i32 %N) {
entry:
  %tmp = sext i32 %N to i64
  br label %for.cond

for.cond:                                         ; preds = %for.inc4, %entry
; CHECK-LABEL: Stmt_for_cond
; CHECK:       ReadAccess := [Reduction Type: NONE] [Scalar: 1]
; CHECK:           [N] -> { Stmt_for_cond[i0] -> MemRef_x_addr_0__phi[] };
; CHECK-NOT: Access
; CHECK:       MustWriteAccess :=  [Reduction Type: NONE] [Scalar: 1]
; CHECK:           [N] -> { Stmt_for_cond[i0] -> MemRef_x_addr_0[] };
; CHECK-NOT: Access
; CHECK:       MustWriteAccess :=  [Reduction Type: NONE] [Scalar: 1]
; CHECK:           [N] -> { Stmt_for_cond[i0] -> MemRef_x_addr_0[] };
; CHECK-NOT: Access
  %indvars.iv = phi i64 [ %indvars.iv.next, %for.inc4 ], [ 1, %entry ]
  %x.addr.0 = phi i32 [ %x, %entry ], [ %x.addr.1.lcssa, %for.inc4 ]
  %cmp = icmp slt i64 %indvars.iv, %tmp
  br i1 %cmp, label %for.body, label %for.end6

for.body:                                         ; preds = %for.cond
; CHECK-LABEL: Stmt_for_body
; CHECK:       ReadAccess :=  [Reduction Type: NONE] [Scalar: 1]
; CHECK:           [N] -> { Stmt_for_body[i0] -> MemRef_x_addr_0[] };
; CHECK-NOT: Access
; CHECK:       MustWriteAccess :=  [Reduction Type: NONE] [Scalar: 1]
; CHECK:           [N] -> { Stmt_for_body[i0] -> MemRef_x_addr_1__phi[] };
; CHECK-NOT: Access
  br label %for.cond1

for.cond1:                                        ; preds = %for.inc, %for.body
; CHECK-LABEL: Stmt_for_cond1
; CHECK:       ReadAccess := [Reduction Type: NONE] [Scalar: 1]
; CHECK:           [N] -> { Stmt_for_cond1[i0, i1] -> MemRef_x_addr_1__phi[] };
; CHECK-NOT: Access
; CHECK:       MustWriteAccess :=  [Reduction Type: NONE] [Scalar: 1]
; CHECK:           [N] -> { Stmt_for_cond1[i0, i1] -> MemRef_x_addr_1[] };
; CHECK-NOT: Access
; CHECK:       MustWriteAccess :=  [Reduction Type: NONE] [Scalar: 1]
; CHECK:           [N] -> { Stmt_for_cond1[i0, i1] -> MemRef_x_addr_1_lcssa__phi[] };
; CHECK-NOT: Access
  %x.addr.1 = phi i32 [ %x.addr.0, %for.body ], [ %add, %for.inc ]
  %j.0 = phi i32 [ 3, %for.body ], [ %inc, %for.inc ]
  %exitcond = icmp ne i32 %j.0, %N
  br i1 %exitcond, label %for.body3, label %for.end

for.body3:                                        ; preds = %for.cond1
  br label %for.inc

for.inc:                                          ; preds = %for.body3
; CHECK-LABEL: Stmt_for_inc
; CHECK:       MustWriteAccess :=  [Reduction Type: NONE] [Scalar: 1]
; CHECK:           [N] -> { Stmt_for_inc[i0, i1] -> MemRef_x_addr_1__phi[] };
; CHECK-NOT: Access
; CHECK:       ReadAccess := [Reduction Type: NONE] [Scalar: 1]
; CHECK:           [N] -> { Stmt_for_inc[i0, i1] -> MemRef_x_addr_1[] };
; CHECK-NOT: Access
; CHECK:       ReadAccess := [Reduction Type: NONE] [Scalar: 0]
; CHECK:           [N] -> { Stmt_for_inc[i0, i1] -> MemRef_A[1 + i0] };
; CHECK-NOT: Access
  %arrayidx = getelementptr inbounds i32, i32* %A, i64 %indvars.iv
  %tmp1 = load i32, i32* %arrayidx, align 4
  %add = add nsw i32 %x.addr.1, %tmp1
  %inc = add nsw i32 %j.0, 1
  br label %for.cond1

for.end:                                          ; preds = %for.cond1
; CHECK-LABEL: Stmt_for_end
; CHECK-NOT: Access
; CHECK:       MustWriteAccess :=  [Reduction Type: NONE] [Scalar: 1]
; CHECK:           [N] -> { Stmt_for_end[i0] -> MemRef_x_addr_1_lcssa[] };
; CHECK-NOT: Access
; CHECK:       ReadAccess := [Reduction Type: NONE] [Scalar: 1]
; CHECK:           [N] -> { Stmt_for_end[i0] -> MemRef_x_addr_1_lcssa__phi[] };
; CHECK-NOT: Access
  %x.addr.1.lcssa = phi i32 [ %x.addr.1, %for.cond1 ]
  br label %for.inc4

for.inc4:                                         ; preds = %for.end
; CHECK-LABEL: Stmt_for_inc4
; CHECK-NOT: Access
; CHECK:       ReadAccess := [Reduction Type: NONE] [Scalar: 1]
; CHECK:           [N] -> { Stmt_for_inc4[i0] -> MemRef_x_addr_1_lcssa[] };
; CHECK-NOT: Access
; CHECK:       MustWriteAccess :=  [Reduction Type: NONE] [Scalar: 1]
; CHECK:           [N] -> { Stmt_for_inc4[i0] -> MemRef_x_addr_0__phi[] };
; CHECK-NOT: Access
  %indvars.iv.next = add nuw nsw i64 %indvars.iv, 1
  br label %for.cond

for.end6:                                         ; preds = %for.cond
  ret i32 %x.addr.0
}
