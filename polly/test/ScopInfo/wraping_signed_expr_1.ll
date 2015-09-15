; RUN: opt %loadPolly -polly-detect-unprofitable -polly-scops -analyze < %s | FileCheck %s
;
;    void f(long *A, long N, long p) {
;      for (long i = 0; i < N; i++)
;        A[i + 1] = 0;
;    }
;
; The wrap function has no inbounds GEP but the nowrap function has. Therefore,
; we will add the assumption that i+1 won't overflow only to the former.
;
; Note:
;       1152921504606846975 * sizeof(long) <= 2 ^ 63 - 1
;  and
;       1152921504606846976 * sizeof(long) >  2 ^ 63 - 1
; with
;       sizeof(long) == 8
;
; CHECK:      Function: wrap
; CHECK:      Boundary Context:
; CHECK:      [N] -> {  : N <= 1152921504606846975 }
;
; CHECK:      Function: nowrap
; CHECK:      Boundary Context:
; CHECK:      [N] -> {  :  }
;
target datalayout = "e-m:e-i64:64-f80:128-n8:16:32:64-S128"

define void @wrap(i64* %A, i64 %N, i64 %p) {
bb:
  br label %bb2

bb2:                                              ; preds = %bb7, %bb
  %indvars.iv = phi i64 [ %indvars.iv.next, %bb7 ], [ 0, %bb ]
  %tmp3 = icmp slt i64 %indvars.iv, %N
  br i1 %tmp3, label %bb4, label %bb8

bb4:                                              ; preds = %bb2
  %tmp5 = add nsw nuw i64 %indvars.iv, 1
  %tmp6 = getelementptr i64, i64* %A, i64 %tmp5
  store i64 0, i64* %tmp6, align 4
  br label %bb7

bb7:                                              ; preds = %bb4
  %indvars.iv.next = add nsw nuw i64 %indvars.iv, 1
  br label %bb2

bb8:                                              ; preds = %bb2
  ret void
}

define void @nowrap(i64* %A, i64 %N, i64 %p) {
bb:
  br label %bb2

bb2:                                              ; preds = %bb7, %bb
  %indvars.iv = phi i64 [ %indvars.iv.next, %bb7 ], [ 0, %bb ]
  %tmp3 = icmp slt i64 %indvars.iv, %N
  br i1 %tmp3, label %bb4, label %bb8

bb4:                                              ; preds = %bb2
  %tmp5 = add nsw nuw i64 %indvars.iv, 1
  %tmp6 = getelementptr inbounds i64, i64* %A, i64 %tmp5
  store i64 0, i64* %tmp6, align 4
  br label %bb7

bb7:                                              ; preds = %bb4
  %indvars.iv.next = add nsw nuw i64 %indvars.iv, 1
  br label %bb2

bb8:                                              ; preds = %bb2
  ret void
}
