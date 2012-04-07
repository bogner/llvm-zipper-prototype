; RUN: opt %loadPolly %defaultOpts -polly-codegen -enable-polly-vector -dce -S %s | FileCheck %s
; RUN: opt %loadPolly -basicaa -polly-codegen -enable-polly-vector -enable-polly-grouped-unroll -S %s | FileCheck -check-prefix=UNROLL %s
; RUN: opt %loadPolly %defaultOpts -polly-import-jscop -polly-import-jscop-dir=%S -polly-cloog -analyze   %s | FileCheck -check-prefix=IMPORT %s
; RUN: opt %loadPolly %defaultOpts -polly-import-jscop -polly-import-jscop-dir=%S -polly-codegen  %s -S  -enable-polly-vector | FileCheck -check-prefix=CODEGEN %s
; ModuleID = 'simple_vec_stride_one.s'
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64"
target triple = "x86_64-unknown-linux-gnu"

@A = common global [1024 x float] zeroinitializer, align 16
@B = common global [1024 x float] zeroinitializer, align 16

define void @simple_vec_stride_one() nounwind {
; <label>:0
  br label %1

; <label>:1                                       ; preds = %4, %0
  %indvar = phi i64 [ %indvar.next, %4 ], [ 0, %0 ]
  %scevgep = getelementptr [1024 x float]* @B, i64 0, i64 %indvar
  %scevgep1 = getelementptr [1024 x float]* @A, i64 0, i64 %indvar
  %exitcond = icmp ne i64 %indvar, 4
  br i1 %exitcond, label %2, label %5

; <label>:2                                       ; preds = %1
  %3 = load float* %scevgep1, align 4
  store float %3, float* %scevgep, align 4
  br label %4

; <label>:4                                       ; preds = %2
  %indvar.next = add i64 %indvar, 1
  br label %1

; <label>:5                                       ; preds = %1
  ret void
}

define i32 @main() nounwind {
  call void @simple_vec_stride_one()
  %1 = load float* getelementptr inbounds ([1024 x float]* @A, i64 0, i64 42), align 8
  %2 = fptosi float %1 to i32
  ret i32 %2
}

; CHECK: bitcast float* {{.*}} to <4 x float>*
; CHECK: load <4 x float>*
; CHECK: store <4 x float> %_p_vec_full, <4 x float>* %vector_ptr

; IMPORT: for (c2=0;c2<=12;c2+=4) {
; IMPORT:     Stmt_2(c2/4);
; IMPORT: }

; We do not generate optimal loads for this.
; CODEGEN: <4 x float>


; UNROLL: %p_scevgep1.moved.to. = getelementptr [1024 x float]* @A, i64 0, i64 0
; UNROLL: %p_scevgep1.moved.to.1 = getelementptr [1024 x float]* @A, i64 0, i64 1
; UNROLL: %p_scevgep1.moved.to.2 = getelementptr [1024 x float]* @A, i64 0, i64 2
; UNROLL: %p_scevgep1.moved.to.3 = getelementptr [1024 x float]* @A, i64 0, i64 3
; UNROLL: %p_scevgep.moved.to. = getelementptr [1024 x float]* @B, i64 0, i64 0
; UNROLL: %p_scevgep.moved.to.4 = getelementptr [1024 x float]* @B, i64 0, i64 1
; UNROLL: %p_scevgep.moved.to.5 = getelementptr [1024 x float]* @B, i64 0, i64 2
; UNROLL: %p_scevgep.moved.to.6 = getelementptr [1024 x float]* @B, i64 0, i64 3
; UNROLL: %_p_scalar_ = load float* %p_scevgep1.moved.to.
; UNROLL: %_p_scalar_7 = load float* %p_scevgep1.moved.to.1
; UNROLL: %_p_scalar_8 = load float* %p_scevgep1.moved.to.2
; UNROLL: %_p_scalar_9 = load float* %p_scevgep1.moved.to.3
; UNROLL: store float %_p_scalar_, float* %p_scevgep.moved.to., align 4
; UNROLL: store float %_p_scalar_7, float* %p_scevgep.moved.to.4, align 4
; UNROLL: store float %_p_scalar_8, float* %p_scevgep.moved.to.5, align 4
; UNROLL: store float %_p_scalar_9, float* %p_scevgep.moved.to.6, align 4
; UNROLL: br label %polly.merge_new_and_old
