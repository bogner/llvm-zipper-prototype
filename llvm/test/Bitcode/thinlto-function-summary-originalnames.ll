; Test to check the callgraph in summary
; RUN: opt -module-summary %s -o %t.o
; RUN: llvm-lto -thinlto-action=thinlink -o %t.index.bc %t.o
; RUN: llvm-bcanalyzer -dump %t.index.bc | FileCheck %s --check-prefix=COMBINED

; COMBINED:       <GLOBALVAL_SUMMARY_BLOCK
; COMBINED-NEXT:    <COMBINED
; COMBINED-NEXT:    <COMBINED_ORIGINAL_NAME op0=6699318081062747564/>
; COMBINED-NEXT:    <COMBINED_GLOBALVAR_INIT_REFS
; COMBINED-NEXT:    <COMBINED_ORIGINAL_NAME op0=-2012135647395072713/>
; COMBINED-NEXT:    <COMBINED_ALIAS
; COMBINED-NEXT:    <COMBINED_ORIGINAL_NAME op0=-4170563161550796836/>
; COMBINED-NEXT:  </GLOBALVAL_SUMMARY_BLOCK>

; ModuleID = 'thinlto-function-summary-callgraph.ll'
target datalayout = "e-m:e-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

@bar = internal global i32 0
@fooalias = internal alias void (...), bitcast (void ()* @foo to void (...)*)

define internal void @foo() {
    ret void
}
