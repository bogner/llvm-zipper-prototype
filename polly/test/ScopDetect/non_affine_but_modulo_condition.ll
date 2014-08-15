; RUN: opt %loadPolly -polly-detect -analyze < %s | FileCheck %s
;
; CHECK: jd_and
; CHECK: Valid Region for Scop:
; CHECK: jd_srem
; CHECK: Valid Region for Scop:
;
;    void jd(int *A) {
;      for (int i = 0; i < 1024; i++)
;        if (i % 2)
;          A[i] += 1;
;    }
;
target datalayout = "e-m:e-i64:64-f80:128-n8:16:32:64-S128"

define void @jd_and(i32* %A) {
entry:
  br label %for.cond

for.cond:                                         ; preds = %for.inc, %entry
  %indvars.iv = phi i64 [ %indvars.iv.next, %for.inc ], [ 0, %entry ]
  %exitcond = icmp ne i64 %indvars.iv, 1024
  br i1 %exitcond, label %for.body, label %for.end

for.body:                                         ; preds = %for.cond
  %tmp = trunc i64 %indvars.iv to i32
  %rem1 = and i32 %tmp, 1
  %tobool = icmp eq i32 %rem1, 0
  br i1 %tobool, label %if.end, label %if.then

if.then:                                          ; preds = %for.body
  %arrayidx = getelementptr inbounds i32* %A, i64 %indvars.iv
  %tmp2 = load i32* %arrayidx, align 4
  %add = add nsw i32 %tmp2, 1
  store i32 %add, i32* %arrayidx, align 4
  br label %if.end

if.end:                                           ; preds = %for.body, %if.then
  br label %for.inc

for.inc:                                          ; preds = %if.end
  %indvars.iv.next = add nuw nsw i64 %indvars.iv, 1
  br label %for.cond

for.end:                                          ; preds = %for.cond
  ret void
}

define void @jd_srem(i32* %A) {
entry:
  br label %for.cond

for.cond:                                         ; preds = %for.inc, %entry
  %indvars.iv = phi i64 [ %indvars.iv.next, %for.inc ], [ 0, %entry ]
  %exitcond = icmp ne i64 %indvars.iv, 1024
  br i1 %exitcond, label %for.body, label %for.end

for.body:                                         ; preds = %for.cond
  %tmp = trunc i64 %indvars.iv to i32
  %rem1 = srem i32 %tmp, 2
  %tobool = icmp eq i32 %rem1, 0
  br i1 %tobool, label %if.end, label %if.then

if.then:                                          ; preds = %for.body
  %arrayidx = getelementptr inbounds i32* %A, i64 %indvars.iv
  %tmp2 = load i32* %arrayidx, align 4
  %add = add nsw i32 %tmp2, 1
  store i32 %add, i32* %arrayidx, align 4
  br label %if.end

if.end:                                           ; preds = %for.body, %if.then
  br label %for.inc

for.inc:                                          ; preds = %if.end
  %indvars.iv.next = add nuw nsw i64 %indvars.iv, 1
  br label %for.cond

for.end:                                          ; preds = %for.cond
  ret void
}
