; RUN: opt %loadPolly -polly-codegen -enable-polly-openmp -S %s | FileCheck %s
;
; 'arg' is converted to float before the loop, so the corresponding temporary
; has to be copied to the function generated for the loop.
;
; float A[100];
; void copy_in_test(long arg) {
;   long i;
;   for (i=0; i<100; ++i)
;     A[i] = arg;
; }

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

@A = common global [100 x float] zeroinitializer, align 16

define void @copy_in_test(i64 %arg) nounwind uwtable {
entry:
  %conv = sitofp i64 %arg to float
  br label %for.body

for.body:                                         ; preds = %for.body, %entry
  %indvars.iv = phi i64 [ 0, %entry ], [ %indvars.iv.next, %for.body ]
  %arrayidx = getelementptr inbounds [100 x float]* @A, i64 0, i64 %indvars.iv
  store float %conv, float* %arrayidx
  %indvars.iv.next = add i64 %indvars.iv, 1
  %exitcond = icmp eq i64 %indvars.iv.next, 100
  br i1 %exitcond, label %for.end, label %for.body

for.end:                                          ; preds = %for.body
  ret void
}

; CHECK: %omp.userContext = alloca { [100 x float]*, float }
