; RUN: llvm-link -S -o - %s %S/Inputs/dicompositetype-unique.ll \
; RUN:   | FileCheck %s
; RUN: llvm-link -S -o - %S/Inputs/dicompositetype-unique.ll %s \
; RUN:   | FileCheck %s -check-prefix REVERSE
; RUN: llvm-link -disable-debug-info-type-map -S -o - %s %S/Inputs/dicompositetype-unique.ll \
; RUN:   | FileCheck %s -check-prefix NOMAP

; Check that the bitcode reader handles this too.
; RUN: llvm-as -o %t1.bc <%s
; RUN: llvm-as -o %t2.bc <%S/Inputs/dicompositetype-unique.ll
; RUN: llvm-link -S -o - %t1.bc %t2.bc | FileCheck %s
; RUN: llvm-link -S -o - %t2.bc %t1.bc | FileCheck %s -check-prefix REVERSE
; RUN: llvm-link -disable-debug-info-type-map -S -o - %t1.bc %t2.bc \
; RUN:   | FileCheck %s -check-prefix NOMAP

; Check that the type map will unique two DICompositeTypes.

; CHECK:   !named = !{!0, !1, !0, !1}
; REVERSE: !named = !{!0, !1, !0, !1}
; NOMAP:   !named = !{!0, !1, !0, !2}
!named = !{!0, !1}

; Check both directions.
; CHECK:        !1 = !DICompositeType(
; CHECK-SAME:                         name: "T1"
; CHECK-SAME:                         identifier: "T"
; CHECK-NOT:       identifier: "T"
; REVERSE:      !1 = !DICompositeType(
; REVERSE-SAME:                       name: "T2"
; REVERSE-SAME:                       identifier: "T"
; REVERSE-NOT:     identifier: "T"

; These types are different, so we should get both copies when there is no map.
; NOMAP:        !1 = !DICompositeType(
; NOMAP-SAME:                         name: "T1"
; NOMAP-SAME:                         identifier: "T"
; NOMAP:        !2 = !DICompositeType(
; NOMAP-SAME:                         name: "T2"
; NOMAP-SAME:                         identifier: "T"
; NOMAP-NOT:       identifier: "T"
!0 = !DIFile(filename: "abc", directory: "/path/to")
!1 = !DICompositeType(tag: DW_TAG_class_type, name: "T1", identifier: "T", file: !0)
