; RUN: opt %loadPolly -polly-detect-unprofitable -basicaa -polly-independent -S < %s | FileCheck %s -check-prefix=SCALARACCESS
; RAUN: opt %loadPolly -polly-detect-unprofitable -basicaa -polly-independent < %s -S | FileCheck %s -check-prefix=SCALARACCESS

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64"

@A = common global [1024 x float] zeroinitializer, align 8

define i32 @empty() nounwind {
entry:
  fence seq_cst
  br label %for.cond

for.cond:
  %indvar = phi i64 [ %indvar.next, %for.inc ], [ 0, %entry ]
  %exitcond = icmp ne i64 %indvar, 1024
  br i1 %exitcond, label %for.body, label %return

for.body:
  br label %for.inc

for.inc:
  %indvar.next = add i64 %indvar, 1
  br label %for.cond

return:
  fence seq_cst
  ret i32 0
}


; SCALARACCESS-LABEL: @array_access()
define i32 @array_access() nounwind {
entry:
  fence seq_cst
  br label %for.cond
; SCALARACCESS: entry:
; SCALARACCESS-NOT: alloca

for.cond:
  %indvar = phi i64 [ %indvar.next, %for.inc ], [ 0, %entry ]
  %exitcond = icmp ne i64 %indvar, 1024
  br i1 %exitcond, label %for.body, label %return

for.body:
  %arrayidx = getelementptr [1024 x float], [1024 x float]* @A, i64 0, i64 %indvar
  %float = uitofp i64 %indvar to float
  store float %float, float* %arrayidx
  br label %for.inc

; SCALARACCESS: for.body:
; SCALARACCESS: %float = uitofp i64 %indvar to float
; SCALARACCESS: store float %float, float* %arrayidx

for.inc:
  %indvar.next = add i64 %indvar, 1
  br label %for.cond

return:
  fence seq_cst
  ret i32 0
}

; SCALARACCESS-LABEL: @intra_scop_dep()
define i32 @intra_scop_dep() nounwind {
entry:
  fence seq_cst
  br label %for.cond

; SCALARACCESS: entry:
; SCALARACCESS-NOT: alloca
; SCALARACCESS: fence

for.cond:
  %indvar = phi i64 [ %indvar.next, %for.inc ], [ 0, %entry ]
  %exitcond = icmp ne i64 %indvar, 1024
  br i1 %exitcond, label %for.body.a, label %return

for.body.a:
  %arrayidx = getelementptr [1024 x float], [1024 x float]* @A, i64 0, i64 %indvar
  %scalar = load float, float* %arrayidx
  br label %for.body.b

; SCALARACCESS: for.body.a:
; SCALARACCESS: %arrayidx = getelementptr [1024 x float], [1024 x float]* @A, i64 0, i64 %indvar
; SCALARACCESS: %scalar = load float, float* %arrayidx
; SCALARACCESS-NOT: store
; SCALARACCESS: br label %for.body.b

for.body.b:
  %arrayidx2 = getelementptr [1024 x float], [1024 x float]* @A, i64 0, i64 %indvar
  %float = uitofp i64 %indvar to float
  %sum = fadd float %scalar, %float
  store float %sum, float* %arrayidx2
  br label %for.inc

; SCALARACCESS: for.body.b:
; SCALARACCESS: %arrayidx2 = getelementptr [1024 x float], [1024 x float]* @A, i64 0, i64 %indvar
; SCALARACCESS: %float = uitofp i64 %indvar to float
; SCALARACCESS-NOT: load
; SCALARACCESS: %sum = fadd float %scalar, %float
; SCALARACCESS: store float %sum, float* %arrayidx2
; SCALARACCESS: br label %for.inc

for.inc:
  %indvar.next = add i64 %indvar, 1
  br label %for.cond

return:
  fence seq_cst
  ret i32 0
}

; It is not possible to have a scop which accesses a scalar element that is
; a global variable. All global variables are pointers containing possibly
; a single element.

; SCALARACCESS-LABEL: @use_after_scop()
define i32 @use_after_scop() nounwind {
entry:
  fence seq_cst
  br label %for.head

; SCALARACCESS: entry:
; SCALARACCESS-NOT: alloca
; SCALARACCESS: fence

for.head:
  %indvar = phi i64 [ %indvar.next, %for.inc ], [ 0, %entry ]
  br label %for.body

for.body:
  %arrayidx = getelementptr [1024 x float], [1024 x float]* @A, i64 0, i64 %indvar
  %scalar = load float, float* %arrayidx
  br label %for.inc

; SCALARACCESS: for.body:
; SCALARACCESS: %scalar = load float, float* %arrayidx
; SCALARACCESS-NOT: store float %scalar

for.inc:
  %indvar.next = add i64 %indvar, 1
  %exitcond = icmp ne i64 %indvar, 1024
  br i1 %exitcond, label %for.head, label %for.after

for.after:
  fence seq_cst
  %return_value = fptosi float %scalar to i32
  br label %return

; SCALARACCESS: for.after:
; SCALARACCESS: fence seq_cst
; SCALARACCESS: %return_value = fptosi float %scalar to i32

return:
  ret i32 %return_value
}

; We currently do not transform scalar references, that have only read accesses
; in the scop. There are two reasons for this:
;
;  o We don't introduce additional memory references which may yield to compile
;    time overhead.
;  o For integer values, such a translation may block the use of scalar
;    evolution on those values.
;
; SCALARACCESS-LABEL: @before_scop()
define i32 @before_scop() nounwind {
entry:
  br label %preheader

preheader:
  %scalar = fadd float 4.0, 5.0
  fence seq_cst
  br label %for.cond

for.cond:
  %indvar = phi i64 [ %indvar.next, %for.inc ], [ 0, %preheader ]
  %exitcond = icmp ne i64 %indvar, 1024
  br i1 %exitcond, label %for.body, label %return

for.body:
  %arrayidx = getelementptr [1024 x float], [1024 x float]* @A, i64 0, i64 %indvar
  store float %scalar, float* %arrayidx
  br label %for.inc

; SCALARACCESS: for.body:
; SCALARACCESS: store float %scalar, float* %arrayidx

for.inc:
  %indvar.next = add i64 %indvar, 1
  br label %for.cond

return:
  fence seq_cst
  ret i32 0
}

; Currently not working
; SCALARACCESS-LABEL: @param_before_scop(
define i32 @param_before_scop(float %scalar) nounwind {
entry:
  fence seq_cst
  br label %for.cond

for.cond:
  %indvar = phi i64 [ %indvar.next, %for.inc ], [ 0, %entry ]
  %exitcond = icmp ne i64 %indvar, 1024
  br i1 %exitcond, label %for.body, label %return

for.body:
  %arrayidx = getelementptr [1024 x float], [1024 x float]* @A, i64 0, i64 %indvar
  store float %scalar, float* %arrayidx
  br label %for.inc

for.inc:
  %indvar.next = add i64 %indvar, 1
  br label %for.cond

return:
  fence seq_cst
  ret i32 0
}
