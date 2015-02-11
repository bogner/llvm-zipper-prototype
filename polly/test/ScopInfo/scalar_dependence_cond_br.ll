; RUN: opt %loadPolly -polly-scops -disable-polly-intra-scop-scalar-to-array -polly-model-phi-nodes -analyze < %s | FileCheck %s
; XFAIL: *
;
;    void f(int *A, int c, int d) {
;      for (int i = 0; i < 1024; i++)
;        if (c < i)
;          A[i]++;
;    }
;
; CHECK:      Stmt_for_cond
; CHECK:            MustWriteAccess :=  [Reduction Type: NONE] [Scalar: 1]
; CHECK:                [c] -> { Stmt_for_cond[i0] -> MemRef_cmp1[] };
; CHECK:      Stmt_for_body
; CHECK:            ReadAccess := [Reduction Type: NONE] [Scalar: 1]
; CHECK:                [c] -> { Stmt_for_body[i0] -> MemRef_cmp1[] };
;
target datalayout = "e-m:e-i64:64-f80:128-n8:16:32:64-S128"

define void @f(i32* %A, i64 %c) {
entry:
  br label %for.cond

for.cond:                                         ; preds = %for.inc, %entry
  %indvars.iv = phi i64 [ %indvars.iv.next, %for.inc ], [ 0, %entry ]
  %exitcond = icmp ne i64 %indvars.iv, 1024
  %cmp1 = icmp slt i64 %c, %indvars.iv
  br i1 %exitcond, label %for.body, label %for.end

for.body:                                         ; preds = %for.cond
  br i1 %cmp1, label %if.then, label %if.end

if.then:                                          ; preds = %for.body
  %arrayidx = getelementptr inbounds i32* %A, i64 %indvars.iv
  %tmp = load i32* %arrayidx, align 4
  %inc = add nsw i32 %tmp, 1
  store i32 %inc, i32* %arrayidx, align 4
  br label %if.end

if.end:                                           ; preds = %if.then, %for.body
  br label %for.inc

for.inc:                                          ; preds = %if.end
  %indvars.iv.next = add nuw nsw i64 %indvars.iv, 1
  br label %for.cond

for.end:                                          ; preds = %for.cond
  ret void
}
