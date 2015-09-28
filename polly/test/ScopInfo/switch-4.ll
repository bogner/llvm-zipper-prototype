; RUN: opt %loadPolly -polly-detect-unprofitable -polly-scops -analyze < %s | FileCheck %s
; RUN: opt %loadPolly -polly-detect-unprofitable -polly-ast -analyze < %s | FileCheck %s --check-prefix=AST
;
;    void f(int *A, int N) {
;      for (int i = 0; i < N; i++)
;        switch (i % 4) {
;        case 0:
;          A[i] += 1;
;          break;
;        case 1:
;          A[i] += 2;
;          break;
;        case 2:
;          A[i] += 3;
;          break;
;        case 3:
;          A[i] += 4;
;          break;
;        default:
;          A[i - 1] += A[i + 1];
;        }
;    }
;
; CHECK:    Statements {
; CHECK:      Stmt_sw_bb_9
; CHECK:            Domain :=
; CHECK:                [N] -> { Stmt_sw_bb_9[i0] : exists (e0 = floor((-3 + i0)/4): 4e0 = -3 + i0 and i0 >= 0 and i0 <= -1 + N) };
; CHECK:            Schedule :=
; CHECK:                [N] -> { Stmt_sw_bb_9[i0] -> [i0, 0] };
; CHECK:      Stmt_sw_bb_5
; CHECK:            Domain :=
; CHECK:                [N] -> { Stmt_sw_bb_5[i0] : exists (e0 = floor((-2 + i0)/4): 4e0 = -2 + i0 and i0 >= 2 and i0 <= -1 + N) };
; CHECK:            Schedule :=
; CHECK:                [N] -> { Stmt_sw_bb_5[i0] -> [i0, 1] };
; CHECK:      Stmt_sw_bb_1
; CHECK:            Domain :=
; CHECK:                [N] -> { Stmt_sw_bb_1[i0] : exists (e0 = floor((-1 + i0)/4): 4e0 = -1 + i0 and i0 >= 1 and i0 <= -1 + N) };
; CHECK:            Schedule :=
; CHECK:                [N] -> { Stmt_sw_bb_1[i0] -> [i0, 2] };
; CHECK:      Stmt_sw_bb
; CHECK:            Domain :=
; CHECK:                [N] -> { Stmt_sw_bb[i0] : exists (e0 = floor((i0)/4): 4e0 = i0 and i0 >= 0 and i0 <= -1 + N) };
; CHECK:            Schedule :=
; CHECK:                [N] -> { Stmt_sw_bb[i0] -> [i0, 3] };
; CHECK:      Stmt_sw_default
; CHECK:            Domain :=
; CHECK:                [N] -> { Stmt_sw_default[i0] : 1 = 0 };
; CHECK:    }
;
; AST:  if (1)
;
; AST:      {
; AST:        for (int c0 = 0; c0 < N - 3; c0 += 4) {
; AST:          Stmt_sw_bb(c0);
; AST:          Stmt_sw_bb_1(c0 + 1);
; AST:          Stmt_sw_bb_5(c0 + 2);
; AST:          Stmt_sw_bb_9(c0 + 3);
; AST:        }
; AST:        if (N >= 1)
; AST:          if (N % 4 >= 1) {
; AST:            Stmt_sw_bb(-((N - 1) % 4) + N - 1);
; AST:            if (N % 4 >= 2) {
; AST:              Stmt_sw_bb_1(-((N - 1) % 4) + N);
; AST:              if ((N - 3) % 4 == 0)
; AST:                Stmt_sw_bb_5(N - 1);
; AST:            }
; AST:          }
; AST:      }
;
; AST:  else
; AST:      {  /* original code */ }
;
target datalayout = "e-m:e-i64:64-f80:128-n8:16:32:64-S128"

define void @f(i32* %A, i32 %N) {
entry:
  %tmp = sext i32 %N to i64
  br label %for.cond

for.cond:                                         ; preds = %for.inc, %entry
  %indvars.iv = phi i64 [ %indvars.iv.next, %for.inc ], [ 0, %entry ]
  %cmp = icmp slt i64 %indvars.iv, %tmp
  br i1 %cmp, label %for.body, label %for.end

for.body:                                         ; preds = %for.cond
  %tmp3 = trunc i64 %indvars.iv to i32
  %rem = srem i32 %tmp3, 4
  switch i32 %rem, label %sw.default [
    i32 0, label %sw.bb
    i32 1, label %sw.bb.1
    i32 2, label %sw.bb.5
    i32 3, label %sw.bb.9
  ]

sw.bb:                                            ; preds = %for.body
  %arrayidx = getelementptr inbounds i32, i32* %A, i64 %indvars.iv
  %tmp4 = load i32, i32* %arrayidx, align 4
  %add = add nsw i32 %tmp4, 1
  store i32 %add, i32* %arrayidx, align 4
  br label %sw.epilog

sw.bb.1:                                          ; preds = %for.body
  %arrayidx3 = getelementptr inbounds i32, i32* %A, i64 %indvars.iv
  %tmp5 = load i32, i32* %arrayidx3, align 4
  %add4 = add nsw i32 %tmp5, 2
  store i32 %add4, i32* %arrayidx3, align 4
  br label %sw.epilog

sw.bb.5:                                          ; preds = %for.body
  %arrayidx7 = getelementptr inbounds i32, i32* %A, i64 %indvars.iv
  %tmp6 = load i32, i32* %arrayidx7, align 4
  %add8 = add nsw i32 %tmp6, 3
  store i32 %add8, i32* %arrayidx7, align 4
  br label %sw.epilog

sw.bb.9:                                          ; preds = %for.body
  %arrayidx11 = getelementptr inbounds i32, i32* %A, i64 %indvars.iv
  %tmp7 = load i32, i32* %arrayidx11, align 4
  %add12 = add nsw i32 %tmp7, 4
  store i32 %add12, i32* %arrayidx11, align 4
  br label %sw.epilog

sw.default:                                       ; preds = %for.body
  %tmp8 = add nuw nsw i64 %indvars.iv, 1
  %arrayidx15 = getelementptr inbounds i32, i32* %A, i64 %tmp8
  %tmp9 = load i32, i32* %arrayidx15, align 4
  %tmp10 = add nsw i64 %indvars.iv, -1
  %arrayidx17 = getelementptr inbounds i32, i32* %A, i64 %tmp10
  %tmp11 = load i32, i32* %arrayidx17, align 4
  %add18 = add nsw i32 %tmp11, %tmp9
  store i32 %add18, i32* %arrayidx17, align 4
  br label %sw.epilog

sw.epilog:                                        ; preds = %sw.default, %sw.bb.9, %sw.bb.5, %sw.bb.1, %sw.bb
  br label %for.inc

for.inc:                                          ; preds = %sw.epilog
  %indvars.iv.next = add nuw nsw i64 %indvars.iv, 1
  br label %for.cond

for.end:                                          ; preds = %for.cond
  ret void
}
