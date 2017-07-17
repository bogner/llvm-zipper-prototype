; Test vector minimum on z14.
;
; RUN: llc < %s -mtriple=s390x-linux-gnu -mcpu=z14 | FileCheck %s

declare double @fmin(double, double)
declare double @llvm.minnum.f64(double, double)
declare <2 x double> @llvm.minnum.v2f64(<2 x double>, <2 x double>)

; Test the fmin library function.
define double @f1(double %dummy, double %val1, double %val2) {
; CHECK-LABEL: f1:
; CHECK: wfmindb %f0, %f2, %f4, 4
; CHECK: br %r14
  %ret = call double @fmin(double %val1, double %val2) readnone
  ret double %ret
}

; Test the f64 minnum intrinsic.
define double @f2(double %dummy, double %val1, double %val2) {
; CHECK-LABEL: f2:
; CHECK: wfmindb %f0, %f2, %f4, 4
; CHECK: br %r14
  %ret = call double @llvm.minnum.f64(double %val1, double %val2)
  ret double %ret
}

; Test a f64 constant compare/select resulting in minnum.
define double @f3(double %dummy, double %val) {
; CHECK-LABEL: f3:
; CHECK: lzdr [[REG:%f[0-9]+]]
; CHECK: wfmindb %f0, %f2, [[REG]], 4
; CHECK: br %r14
  %cmp = fcmp olt double %val, 0.0
  %ret = select i1 %cmp, double %val, double 0.0
  ret double %ret
}

; Test a f64 constant compare/select resulting in minnan.
define double @f4(double %dummy, double %val) {
; CHECK-LABEL: f4:
; CHECK: lzdr [[REG:%f[0-9]+]]
; CHECK: wfmindb %f0, %f2, [[REG]], 1
; CHECK: br %r14
  %cmp = fcmp ult double %val, 0.0
  %ret = select i1 %cmp, double %val, double 0.0
  ret double %ret
}

; Test the v2f64 minnum intrinsic.
define <2 x double> @f5(<2 x double> %dummy, <2 x double> %val1,
                        <2 x double> %val2) {
; CHECK-LABEL: f5:
; CHECK: vfmindb %v24, %v26, %v28, 4
; CHECK: br %r14
  %ret = call <2 x double> @llvm.minnum.v2f64(<2 x double> %val1, <2 x double> %val2)
  ret <2 x double> %ret
}

