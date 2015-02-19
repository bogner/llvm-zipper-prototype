; RUN: opt %loadPolly -polly-detect-unprofitable -basicaa -polly-dependences -analyze < %s | FileCheck %s
;
; Verify we do not have dependences between the if and the else clause
;
; CHECK: RAW dependences:
; CHECK:   {  }
; CHECK: WAR dependences:
; CHECK:   {  }
; CHECK: WAW dependences:
; CHECK:   {  }
; CHECK: Reduction dependences:
; CHECK-DAG: Stmt_if_then[i0] -> Stmt_if_then[1 + i0] : i0 <= 510 and i0 >= 0
; CHECK-DAG: Stmt_if_else[i0] -> Stmt_if_else[1 + i0] : i0 <= 1022 and i0 >= 512
;
; void f(int *restrict sum, int *restrict prod) {
;   for (int i = 0; i < 1024; i++)
;     if (i < 512)
;       *sum += i;
;     else
;       *prod *= i;
; }
;
target datalayout = "e-m:e-p:32:32-i64:64-v128:64:128-n32-S64"

define void @f(i32* noalias %sum, i32* noalias %prod)  {
entry:
  br label %for.cond

for.cond:                                         ; preds = %for.inc, %entry
  %i.0 = phi i32 [ 0, %entry ], [ %inc, %for.inc ]
  %exitcond = icmp ne i32 %i.0, 1024
  br i1 %exitcond, label %for.body, label %for.end

for.body:                                         ; preds = %for.cond
  %cmp1 = icmp slt i32 %i.0, 512
  br i1 %cmp1, label %if.then, label %if.else

if.then:                                          ; preds = %for.body
  %tmp = load i32* %sum, align 4
  %add = add nsw i32 %tmp, %i.0
  store i32 %add, i32* %sum, align 4
  br label %if.end

if.else:                                          ; preds = %for.body
  %tmp1 = load i32* %prod, align 4
  %mul = mul nsw i32 %tmp1, %i.0
  store i32 %mul, i32* %prod, align 4
  br label %if.end

if.end:                                           ; preds = %if.else, %if.then
  br label %for.inc

for.inc:                                          ; preds = %if.end
  %inc = add nsw i32 %i.0, 1
  br label %for.cond

for.end:                                          ; preds = %for.cond
  ret void
}
