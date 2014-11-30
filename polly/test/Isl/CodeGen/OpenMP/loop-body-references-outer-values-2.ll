; RUN: opt %loadPolly -polly-parallel -polly-parallel-force -polly-ast -analyze < %s | FileCheck %s -check-prefix=AST
; RUN: opt %loadPolly -polly-parallel -polly-parallel-force -polly-codegen-isl -S -verify-dom-info < %s | FileCheck %s -check-prefix=IR

; AST: #pragma simd
; AST: #pragma omp parallel for
; AST: for (int c1 = 0; c1 <= 1023; c1 += 1)
; AST:   Stmt_for_i(c1);

; IR: getelementptr inbounds { [1024 x double]* }* %polly.par.userContext, i32 0, i32 0

target datalayout = "e-m:e-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

define void @kernel_trmm([1024 x double]* %B) {
entry:
  br label %for.cond1.preheader

for.cond1.preheader:
  %extern = add i64 1, 0
  br label %for.i

for.i:
  %indvar.i = phi i64 [ %indvar.i.next, %for.i ], [ 0, %for.cond1.preheader ]
  %getelementptr = getelementptr [1024 x double]* %B, i64 %extern, i64 %indvar.i
  store double 0.000000e+00, double* %getelementptr
  %indvar.i.next = add i64 %indvar.i, 1
  %exitcond.i = icmp ne i64 %indvar.i.next, 1024
  br i1 %exitcond.i, label %for.i, label %end

end:
  ret void
}
