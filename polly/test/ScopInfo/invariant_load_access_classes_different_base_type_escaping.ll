; RUN: opt %loadPolly -polly-scops -analyze < %s | FileCheck %s
; RUN: opt %loadPolly -polly-codegen -S < %s
;
;    struct {
;      int a;
;      float b;
;    } S;
;
;    float f(int *A) {
;      int x;
;      float y;
;      int i = 0;
;      do {
;        x = S.a;
;        y = S.b;
;        A[i] = x + y;
;      } while (i++ < 1000);
;      return x + y;
;    }
;
; CHECK:   Invariant Accesses: {
; CHECK:            ReadAccess := [Reduction Type: NONE] [Scalar: 0]
; CHECK:                { Stmt_do_body[i0] -> MemRef_S[0] };
; CHECK:            Execution Context: {  :  }
; CHECK:            ReadAccess := [Reduction Type: NONE] [Scalar: 0]
; CHECK:                { Stmt_do_body[i0] -> MemRef_S[1] };
; CHECK:            Execution Context: {  :  }
; CHECK:    }
;
; CHECK:    Statements {
; CHECK-NOT: Access
; CHECK:      Stmt_do_body
; CHECK:            Domain :=
; CHECK:                { Stmt_do_body[i0] : i0 <= 1000 and i0 >= 0 };
; CHECK:            Schedule :=
; CHECK:                { Stmt_do_body[i0] -> [i0] };
; CHECK:            MustWriteAccess :=  [Reduction Type: NONE] [Scalar: 0]
; CHECK:                { Stmt_do_body[i0] -> MemRef_A[i0] };
; CHECK-NOT: Access
; CHECK:    }
;
; CODEGEN: entry:
; CODEGEN:   %S.b.preload.s2a = alloca float
; CODEGEN:   %S.a.preload.s2a = alloca i32
;
; CODEGEN: polly.preload.begin:
; CODEGEN:   %.load = load i32, i32* getelementptr inbounds (%struct.anon, %struct.anon* @S, i32 0, i32 0)
; CODEGEN:   %0 = bitcast i32 %.load to float
; CODEGEN:   store i32 %.load, i32* %S.a.preload.s2a
; CODEGEN:   %.load1 = load i32, i32* getelementptr (i32, i32* getelementptr inbounds (%struct.anon, %struct.anon* @S, i32 0, i32 0), i64 1)
; CODEGEN:   %1 = bitcast i32 %.load1 to float
; CODEGEN:   %2 = bitcast float %1 to i32
; CODEGEN:   store float %1, float* %S.b.preload.s2a
;
; CODEGEN:     polly.merge_new_and_old:
; CODEGEN-DAG:   %S.b.merge = phi float [ %S.b.final_reload, %polly.loop_exit ], [ %S.b, %do.cond ]
; CODEGEN-DAG:   %S.a.merge = phi i32 [ %S.a.final_reload, %polly.loop_exit ], [ %S.a, %do.cond ]
;
; CODEGEN: do.end:
; CODEGEN:   %conv3 = sitofp i32 %S.a.merge to float
; CODEGEN:   %add4 = fadd float %conv3, %S.b.merge
; CODEGEN:   ret float %add4
;
; CODEGEN: polly.loop_exit:
; CODEGEN-DAG:   %S.b.final_reload = load float, float* %S.b.preload.s2a
; CODEGEN-DAG:   %S.a.final_reload = load i32, i32* %S.a.preload.s2a

;
target datalayout = "e-m:e-i64:64-f80:128-n8:16:32:64-S128"

%struct.anon = type { i32, float }

@S = common global %struct.anon zeroinitializer, align 4

define float @f(i32* %A) {
entry:
  br label %do.body

do.body:                                          ; preds = %do.cond, %entry
  %indvars.iv = phi i64 [ %indvars.iv.next, %do.cond ], [ 0, %entry ]
  %S.a = load i32, i32* getelementptr inbounds (%struct.anon, %struct.anon* @S, i64 0, i32 0), align 4
  %S.b = load float, float* getelementptr inbounds (%struct.anon, %struct.anon* @S, i64 0, i32 1), align 4
  %conv = sitofp i32 %S.a to float
  %add = fadd float %conv, %S.b
  %conv1 = fptosi float %add to i32
  %arrayidx = getelementptr inbounds i32, i32* %A, i64 %indvars.iv
  store i32 %conv1, i32* %arrayidx, align 4
  br label %do.cond

do.cond:                                          ; preds = %do.body
  %indvars.iv.next = add nuw nsw i64 %indvars.iv, 1
  %exitcond = icmp ne i64 %indvars.iv.next, 1001
  br i1 %exitcond, label %do.body, label %do.end

do.end:                                           ; preds = %do.cond
  %conv3 = sitofp i32 %S.a to float
  %add4 = fadd float %conv3, %S.b
  ret float %add4
}
