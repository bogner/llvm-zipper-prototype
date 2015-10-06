; RUN: opt %loadPolly -polly-ast -analyze < %s | FileCheck %s
; RUN: opt %loadPolly -polly-codegen -S < %s | FileCheck %s -check-prefix=CODEGEN
;
; TODO: FIXME: IslExprBuilder is not capable of producing valid code
;              for arbitrary pointer expressions at the moment. Until
;              this is fixed we disallow pointer expressions completely.
; XFAIL: *

;    void f(int a[], int N, float *P, float *Q) {
;      int i;
;      for (i = 0; i < N; ++i)
;        if (P != Q)
;          a[i] = i;
;    }
;
target datalayout = "e-m:e-i64:64-f80:128-n8:16:32:64-S128"

define void @f(i64* nocapture %a, i64 %N, float * %P, float * %Q) nounwind {
entry:
  br label %bb

bb:
  %i = phi i64 [ 0, %entry ], [ %i.inc, %bb.backedge ]
  %brcond = icmp ne float* %P, %Q
  br i1 %brcond, label %store, label %bb.backedge

store:
  %scevgep = getelementptr i64, i64* %a, i64 %i
  store i64 %i, i64* %scevgep
  br label %bb.backedge

bb.backedge:
  %i.inc = add nsw i64 %i, 1
  %exitcond = icmp eq i64 %i.inc, %N
  br i1 %exitcond, label %return, label %bb

return:
  ret void
}

; CHECK: if (Q >= P + 1) {
; CHECK:   for (int c0 = 0; c0 < N; c0 += 1)
; CHECK:     Stmt_store(c0);
; CHECK: } else if (P >= Q + 1)
; CHECK:   for (int c0 = 0; c0 < N; c0 += 1)
; CHECK:     Stmt_store(c0);
; CHECK: }

; CODEGEN:       %[[Pinc:[_a-zA-Z0-9]+]] = getelementptr float, float* %P, i64 1
; CODEGEN-NEXT:                             icmp uge float* %Q, %[[Pinc]]
; CODEGEN:       %[[Qinc:[_a-zA-Z0-9]+]] = getelementptr float, float* %Q, i64 1
; CODEGEN-NEXT:                             icmp uge float* %P, %[[Qinc]]
