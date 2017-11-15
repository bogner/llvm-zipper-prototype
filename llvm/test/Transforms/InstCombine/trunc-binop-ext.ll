; RUN: opt < %s -instcombine -S | FileCheck %s

define i16 @narrow_sext_and(i16 %x16, i32 %y32) {
; CHECK-LABEL: @narrow_sext_and(
; CHECK-NEXT:    [[X321:%.*]] = zext i16 %x16 to i32
; CHECK-NEXT:    [[B:%.*]] = and i32 [[X321]], %y32
; CHECK-NEXT:    [[R:%.*]] = trunc i32 [[B]] to i16
; CHECK-NEXT:    ret i16 [[R]]
;
  %x32 = sext i16 %x16 to i32
  %b = and i32 %x32, %y32
  %r = trunc i32 %b to i16
  ret i16 %r
}

define i16 @narrow_zext_and(i16 %x16, i32 %y32) {
; CHECK-LABEL: @narrow_zext_and(
; CHECK-NEXT:    [[X32:%.*]] = zext i16 %x16 to i32
; CHECK-NEXT:    [[B:%.*]] = and i32 [[X32]], %y32
; CHECK-NEXT:    [[R:%.*]] = trunc i32 [[B]] to i16
; CHECK-NEXT:    ret i16 [[R]]
;
  %x32 = zext i16 %x16 to i32
  %b = and i32 %x32, %y32
  %r = trunc i32 %b to i16
  ret i16 %r
}

define i16 @narrow_sext_or(i16 %x16, i32 %y32) {
; CHECK-LABEL: @narrow_sext_or(
; CHECK-NEXT:    [[X321:%.*]] = zext i16 %x16 to i32
; CHECK-NEXT:    [[B:%.*]] = or i32 [[X321]], %y32
; CHECK-NEXT:    [[R:%.*]] = trunc i32 [[B]] to i16
; CHECK-NEXT:    ret i16 [[R]]
;
  %x32 = sext i16 %x16 to i32
  %b = or i32 %x32, %y32
  %r = trunc i32 %b to i16
  ret i16 %r
}

define i16 @narrow_zext_or(i16 %x16, i32 %y32) {
; CHECK-LABEL: @narrow_zext_or(
; CHECK-NEXT:    [[X32:%.*]] = zext i16 %x16 to i32
; CHECK-NEXT:    [[B:%.*]] = or i32 [[X32]], %y32
; CHECK-NEXT:    [[R:%.*]] = trunc i32 [[B]] to i16
; CHECK-NEXT:    ret i16 [[R]]
;
  %x32 = zext i16 %x16 to i32
  %b = or i32 %x32, %y32
  %r = trunc i32 %b to i16
  ret i16 %r
}

define i16 @narrow_sext_xor(i16 %x16, i32 %y32) {
; CHECK-LABEL: @narrow_sext_xor(
; CHECK-NEXT:    [[X321:%.*]] = zext i16 %x16 to i32
; CHECK-NEXT:    [[B:%.*]] = xor i32 [[X321]], %y32
; CHECK-NEXT:    [[R:%.*]] = trunc i32 [[B]] to i16
; CHECK-NEXT:    ret i16 [[R]]
;
  %x32 = sext i16 %x16 to i32
  %b = xor i32 %x32, %y32
  %r = trunc i32 %b to i16
  ret i16 %r
}

define i16 @narrow_zext_xor(i16 %x16, i32 %y32) {
; CHECK-LABEL: @narrow_zext_xor(
; CHECK-NEXT:    [[X32:%.*]] = zext i16 %x16 to i32
; CHECK-NEXT:    [[B:%.*]] = xor i32 [[X32]], %y32
; CHECK-NEXT:    [[R:%.*]] = trunc i32 [[B]] to i16
; CHECK-NEXT:    ret i16 [[R]]
;
  %x32 = zext i16 %x16 to i32
  %b = xor i32 %x32, %y32
  %r = trunc i32 %b to i16
  ret i16 %r
}

define i16 @narrow_sext_add(i16 %x16, i32 %y32) {
; CHECK-LABEL: @narrow_sext_add(
; CHECK-NEXT:    [[X321:%.*]] = zext i16 %x16 to i32
; CHECK-NEXT:    [[B:%.*]] = add i32 [[X321]], %y32
; CHECK-NEXT:    [[R:%.*]] = trunc i32 [[B]] to i16
; CHECK-NEXT:    ret i16 [[R]]
;
  %x32 = sext i16 %x16 to i32
  %b = add i32 %x32, %y32
  %r = trunc i32 %b to i16
  ret i16 %r
}

define i16 @narrow_zext_add(i16 %x16, i32 %y32) {
; CHECK-LABEL: @narrow_zext_add(
; CHECK-NEXT:    [[X32:%.*]] = zext i16 %x16 to i32
; CHECK-NEXT:    [[B:%.*]] = add i32 [[X32]], %y32
; CHECK-NEXT:    [[R:%.*]] = trunc i32 [[B]] to i16
; CHECK-NEXT:    ret i16 [[R]]
;
  %x32 = zext i16 %x16 to i32
  %b = add i32 %x32, %y32
  %r = trunc i32 %b to i16
  ret i16 %r
}

define i16 @narrow_sext_mul(i16 %x16, i32 %y32) {
; CHECK-LABEL: @narrow_sext_mul(
; CHECK-NEXT:    [[X32:%.*]] = sext i16 %x16 to i32
; CHECK-NEXT:    [[B:%.*]] = mul i32 [[X32]], %y32
; CHECK-NEXT:    [[R:%.*]] = trunc i32 [[B]] to i16
; CHECK-NEXT:    ret i16 [[R]]
;
  %x32 = sext i16 %x16 to i32
  %b = mul i32 %x32, %y32
  %r = trunc i32 %b to i16
  ret i16 %r
}

define i16 @narrow_zext_mul(i16 %x16, i32 %y32) {
; CHECK-LABEL: @narrow_zext_mul(
; CHECK-NEXT:    [[X32:%.*]] = zext i16 %x16 to i32
; CHECK-NEXT:    [[B:%.*]] = mul i32 [[X32]], %y32
; CHECK-NEXT:    [[R:%.*]] = trunc i32 [[B]] to i16
; CHECK-NEXT:    ret i16 [[R]]
;
  %x32 = zext i16 %x16 to i32
  %b = mul i32 %x32, %y32
  %r = trunc i32 %b to i16
  ret i16 %r
}

; Verify that the commuted patterns work. The div is to ensure that complexity-based 
; canonicalization doesn't swap the binop operands. Use vector types to show those work too.

define <2 x i16> @narrow_sext_and_commute(<2 x i16> %x16, <2 x i32> %y32) {
; CHECK-LABEL: @narrow_sext_and_commute(
; CHECK-NEXT:    [[Y32OP0:%.*]] = sdiv <2 x i32> %y32, <i32 7, i32 -17>
; CHECK-NEXT:    [[X321:%.*]] = zext <2 x i16> %x16 to <2 x i32>
; CHECK-NEXT:    [[B:%.*]] = and <2 x i32> [[Y32OP0]], [[X321]]
; CHECK-NEXT:    [[R:%.*]] = trunc <2 x i32> [[B]] to <2 x i16>
; CHECK-NEXT:    ret <2 x i16> [[R]]
;
  %y32op0 = sdiv <2 x i32> %y32, <i32 7, i32 -17>
  %x32 = sext <2 x i16> %x16 to <2 x i32>
  %b = and <2 x i32> %y32op0, %x32
  %r = trunc <2 x i32> %b to <2 x i16>
  ret <2 x i16> %r
}

define <2 x i16> @narrow_zext_and_commute(<2 x i16> %x16, <2 x i32> %y32) {
; CHECK-LABEL: @narrow_zext_and_commute(
; CHECK-NEXT:    [[Y32OP0:%.*]] = sdiv <2 x i32> %y32, <i32 7, i32 -17>
; CHECK-NEXT:    [[X32:%.*]] = zext <2 x i16> %x16 to <2 x i32>
; CHECK-NEXT:    [[B:%.*]] = and <2 x i32> [[Y32OP0]], [[X32]]
; CHECK-NEXT:    [[R:%.*]] = trunc <2 x i32> [[B]] to <2 x i16>
; CHECK-NEXT:    ret <2 x i16> [[R]]
;
  %y32op0 = sdiv <2 x i32> %y32, <i32 7, i32 -17>
  %x32 = zext <2 x i16> %x16 to <2 x i32>
  %b = and <2 x i32> %y32op0, %x32
  %r = trunc <2 x i32> %b to <2 x i16>
  ret <2 x i16> %r
}

define <2 x i16> @narrow_sext_or_commute(<2 x i16> %x16, <2 x i32> %y32) {
; CHECK-LABEL: @narrow_sext_or_commute(
; CHECK-NEXT:    [[Y32OP0:%.*]] = sdiv <2 x i32> %y32, <i32 7, i32 -17>
; CHECK-NEXT:    [[X321:%.*]] = zext <2 x i16> %x16 to <2 x i32>
; CHECK-NEXT:    [[B:%.*]] = or <2 x i32> [[Y32OP0]], [[X321]]
; CHECK-NEXT:    [[R:%.*]] = trunc <2 x i32> [[B]] to <2 x i16>
; CHECK-NEXT:    ret <2 x i16> [[R]]
;
  %y32op0 = sdiv <2 x i32> %y32, <i32 7, i32 -17>
  %x32 = sext <2 x i16> %x16 to <2 x i32>
  %b = or <2 x i32> %y32op0, %x32
  %r = trunc <2 x i32> %b to <2 x i16>
  ret <2 x i16> %r
}

define <2 x i16> @narrow_zext_or_commute(<2 x i16> %x16, <2 x i32> %y32) {
; CHECK-LABEL: @narrow_zext_or_commute(
; CHECK-NEXT:    [[Y32OP0:%.*]] = sdiv <2 x i32> %y32, <i32 7, i32 -17>
; CHECK-NEXT:    [[X32:%.*]] = zext <2 x i16> %x16 to <2 x i32>
; CHECK-NEXT:    [[B:%.*]] = or <2 x i32> [[Y32OP0]], [[X32]]
; CHECK-NEXT:    [[R:%.*]] = trunc <2 x i32> [[B]] to <2 x i16>
; CHECK-NEXT:    ret <2 x i16> [[R]]
;
  %y32op0 = sdiv <2 x i32> %y32, <i32 7, i32 -17>
  %x32 = zext <2 x i16> %x16 to <2 x i32>
  %b = or <2 x i32> %y32op0, %x32
  %r = trunc <2 x i32> %b to <2 x i16>
  ret <2 x i16> %r
}

define <2 x i16> @narrow_sext_xor_commute(<2 x i16> %x16, <2 x i32> %y32) {
; CHECK-LABEL: @narrow_sext_xor_commute(
; CHECK-NEXT:    [[Y32OP0:%.*]] = sdiv <2 x i32> %y32, <i32 7, i32 -17>
; CHECK-NEXT:    [[X321:%.*]] = zext <2 x i16> %x16 to <2 x i32>
; CHECK-NEXT:    [[B:%.*]] = xor <2 x i32> [[Y32OP0]], [[X321]]
; CHECK-NEXT:    [[R:%.*]] = trunc <2 x i32> [[B]] to <2 x i16>
; CHECK-NEXT:    ret <2 x i16> [[R]]
;
  %y32op0 = sdiv <2 x i32> %y32, <i32 7, i32 -17>
  %x32 = sext <2 x i16> %x16 to <2 x i32>
  %b = xor <2 x i32> %y32op0, %x32
  %r = trunc <2 x i32> %b to <2 x i16>
  ret <2 x i16> %r
}

define <2 x i16> @narrow_zext_xor_commute(<2 x i16> %x16, <2 x i32> %y32) {
; CHECK-LABEL: @narrow_zext_xor_commute(
; CHECK-NEXT:    [[Y32OP0:%.*]] = sdiv <2 x i32> %y32, <i32 7, i32 -17>
; CHECK-NEXT:    [[X32:%.*]] = zext <2 x i16> %x16 to <2 x i32>
; CHECK-NEXT:    [[B:%.*]] = xor <2 x i32> [[Y32OP0]], [[X32]]
; CHECK-NEXT:    [[R:%.*]] = trunc <2 x i32> [[B]] to <2 x i16>
; CHECK-NEXT:    ret <2 x i16> [[R]]
;
  %y32op0 = sdiv <2 x i32> %y32, <i32 7, i32 -17>
  %x32 = zext <2 x i16> %x16 to <2 x i32>
  %b = xor <2 x i32> %y32op0, %x32
  %r = trunc <2 x i32> %b to <2 x i16>
  ret <2 x i16> %r
}

define <2 x i16> @narrow_sext_add_commute(<2 x i16> %x16, <2 x i32> %y32) {
; CHECK-LABEL: @narrow_sext_add_commute(
; CHECK-NEXT:    [[Y32OP0:%.*]] = sdiv <2 x i32> %y32, <i32 7, i32 -17>
; CHECK-NEXT:    [[X321:%.*]] = zext <2 x i16> %x16 to <2 x i32>
; CHECK-NEXT:    [[B:%.*]] = add <2 x i32> [[Y32OP0]], [[X321]]
; CHECK-NEXT:    [[R:%.*]] = trunc <2 x i32> [[B]] to <2 x i16>
; CHECK-NEXT:    ret <2 x i16> [[R]]
;
  %y32op0 = sdiv <2 x i32> %y32, <i32 7, i32 -17>
  %x32 = sext <2 x i16> %x16 to <2 x i32>
  %b = add <2 x i32> %y32op0, %x32
  %r = trunc <2 x i32> %b to <2 x i16>
  ret <2 x i16> %r
}

define <2 x i16> @narrow_zext_add_commute(<2 x i16> %x16, <2 x i32> %y32) {
; CHECK-LABEL: @narrow_zext_add_commute(
; CHECK-NEXT:    [[Y32OP0:%.*]] = sdiv <2 x i32> %y32, <i32 7, i32 -17>
; CHECK-NEXT:    [[X32:%.*]] = zext <2 x i16> %x16 to <2 x i32>
; CHECK-NEXT:    [[B:%.*]] = add <2 x i32> [[Y32OP0]], [[X32]]
; CHECK-NEXT:    [[R:%.*]] = trunc <2 x i32> [[B]] to <2 x i16>
; CHECK-NEXT:    ret <2 x i16> [[R]]
;
  %y32op0 = sdiv <2 x i32> %y32, <i32 7, i32 -17>
  %x32 = zext <2 x i16> %x16 to <2 x i32>
  %b = add <2 x i32> %y32op0, %x32
  %r = trunc <2 x i32> %b to <2 x i16>
  ret <2 x i16> %r
}

define <2 x i16> @narrow_sext_mul_commute(<2 x i16> %x16, <2 x i32> %y32) {
; CHECK-LABEL: @narrow_sext_mul_commute(
; CHECK-NEXT:    [[Y32OP0:%.*]] = sdiv <2 x i32> %y32, <i32 7, i32 -17>
; CHECK-NEXT:    [[X32:%.*]] = sext <2 x i16> %x16 to <2 x i32>
; CHECK-NEXT:    [[B:%.*]] = mul <2 x i32> [[Y32OP0]], [[X32]]
; CHECK-NEXT:    [[R:%.*]] = trunc <2 x i32> [[B]] to <2 x i16>
; CHECK-NEXT:    ret <2 x i16> [[R]]
;
  %y32op0 = sdiv <2 x i32> %y32, <i32 7, i32 -17>
  %x32 = sext <2 x i16> %x16 to <2 x i32>
  %b = mul <2 x i32> %y32op0, %x32
  %r = trunc <2 x i32> %b to <2 x i16>
  ret <2 x i16> %r
}

define <2 x i16> @narrow_zext_mul_commute(<2 x i16> %x16, <2 x i32> %y32) {
; CHECK-LABEL: @narrow_zext_mul_commute(
; CHECK-NEXT:    [[Y32OP0:%.*]] = sdiv <2 x i32> %y32, <i32 7, i32 -17>
; CHECK-NEXT:    [[X32:%.*]] = zext <2 x i16> %x16 to <2 x i32>
; CHECK-NEXT:    [[B:%.*]] = mul <2 x i32> [[Y32OP0]], [[X32]]
; CHECK-NEXT:    [[R:%.*]] = trunc <2 x i32> [[B]] to <2 x i16>
; CHECK-NEXT:    ret <2 x i16> [[R]]
;
  %y32op0 = sdiv <2 x i32> %y32, <i32 7, i32 -17>
  %x32 = zext <2 x i16> %x16 to <2 x i32>
  %b = mul <2 x i32> %y32op0, %x32
  %r = trunc <2 x i32> %b to <2 x i16>
  ret <2 x i16> %r
}

