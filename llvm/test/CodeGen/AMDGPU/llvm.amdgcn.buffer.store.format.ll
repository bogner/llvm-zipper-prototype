;RUN: llc < %s -march=amdgcn -mcpu=verde -verify-machineinstrs | FileCheck %s
;RUN: llc < %s -march=amdgcn -mcpu=tonga -verify-machineinstrs | FileCheck %s

;CHECK-LABEL: {{^}}buffer_store:
;CHECK: buffer_store_format_xyzw v[0:3], s[0:3], s4
;CHECK: buffer_store_format_xyzw v[4:7], s[0:3], s4 glc
;CHECK: buffer_store_format_xyzw v[8:11], s[0:3], s4 slc
define void @buffer_store(<4 x i32> inreg, i32 inreg, <4 x float>, <4 x float>, <4 x float>) #0 {
main_body:
  call void @llvm.amdgcn.buffer.store.format.v4f32(<4 x float> %2, <4 x i32> %0, i32 %1, i32 0, i32 0, i32 0, i1 0, i1 0)
  call void @llvm.amdgcn.buffer.store.format.v4f32(<4 x float> %3, <4 x i32> %0, i32 %1, i32 0, i32 0, i32 0, i1 1, i1 0)
  call void @llvm.amdgcn.buffer.store.format.v4f32(<4 x float> %4, <4 x i32> %0, i32 %1, i32 0, i32 0, i32 0, i1 0, i1 1)
  ret void
}

;CHECK-LABEL: {{^}}buffer_store_immoffs:
;CHECK: buffer_store_format_xyzw v[0:3], s[0:3], s4 offset:42
define void @buffer_store_immoffs(<4 x i32> inreg, i32 inreg, <4 x float>) #0 {
main_body:
  call void @llvm.amdgcn.buffer.store.format.v4f32(<4 x float> %2, <4 x i32> %0, i32 %1, i32 42, i32 0, i32 0, i1 0, i1 0)
  ret void
}

;CHECK-LABEL: {{^}}buffer_store_idx:
;CHECK: buffer_store_format_xyzw v[0:3], v4, s[0:3], 0 idxen
define void @buffer_store_idx(<4 x i32> inreg, i32 inreg, <4 x float>, i32) #0 {
main_body:
  call void @llvm.amdgcn.buffer.store.format.v4f32(<4 x float> %2, <4 x i32> %0, i32 0, i32 0, i32 %3, i32 0, i1 0, i1 0)
  ret void
}

;CHECK-LABEL: {{^}}buffer_store_ofs:
;CHECK: buffer_store_format_xyzw v[0:3], v4, s[0:3], 0 offen
define void @buffer_store_ofs(<4 x i32> inreg, i32 inreg, <4 x float>, i32) #0 {
main_body:
  call void @llvm.amdgcn.buffer.store.format.v4f32(<4 x float> %2, <4 x i32> %0, i32 0, i32 0, i32 0, i32 %3, i1 0, i1 0)
  ret void
}

;CHECK-LABEL: {{^}}buffer_store_both:
;CHECK: buffer_store_format_xyzw v[0:3], v[4:5], s[0:3], 0 idxen offen
define void @buffer_store_both(<4 x i32> inreg, i32 inreg, <4 x float>, i32, i32) #0 {
main_body:
  call void @llvm.amdgcn.buffer.store.format.v4f32(<4 x float> %2, <4 x i32> %0, i32 0, i32 0, i32 %3, i32 %4, i1 0, i1 0)
  ret void
}

;CHECK-LABEL: {{^}}buffer_store_both_reversed:
;CHECK: v_mov_b32_e32 v6, v4
;CHECK: buffer_store_format_xyzw v[0:3], v[5:6], s[0:3], 0 idxen offen
define void @buffer_store_both_reversed(<4 x i32> inreg, i32 inreg, <4 x float>, i32, i32) #0 {
main_body:
  call void @llvm.amdgcn.buffer.store.format.v4f32(<4 x float> %2, <4 x i32> %0, i32 0, i32 0, i32 %4, i32 %3, i1 0, i1 0)
  ret void
}

; Ideally, the register allocator would avoid the wait here
;
;CHECK-LABEL: {{^}}buffer_store_wait:
;CHECK: buffer_store_format_xyzw v[0:3], v4, s[0:3], 0 idxen
;CHECK: s_waitcnt vmcnt(0) expcnt(0)
;CHECK: buffer_load_format_xyzw v[0:3], v5, s[0:3], 0 idxen
;CHECK: s_waitcnt vmcnt(0)
;CHECK: buffer_store_format_xyzw v[0:3], v6, s[0:3], 0 idxen
define void @buffer_store_wait(<4 x i32> inreg, i32 inreg, <4 x float>, i32, i32, i32) #0 {
main_body:
  call void @llvm.amdgcn.buffer.store.format.v4f32(<4 x float> %2, <4 x i32> %0, i32 0, i32 0, i32 %3, i32 0, i1 0, i1 0)
  %data = call <4 x float> @llvm.amdgcn.buffer.load.format(<4 x i32> %0, i32 0, i32 0, i32 %4, i32 0, i1 0, i1 0)
  call void @llvm.amdgcn.buffer.store.format.v4f32(<4 x float> %data, <4 x i32> %0, i32 0, i32 0, i32 %5, i32 0, i1 0, i1 0)
  ret void
}

declare void @llvm.amdgcn.buffer.store.format.v4f32(<4 x float>, <4 x i32>, i32, i32, i32, i32, i1, i1) #1
declare <4 x float> @llvm.amdgcn.buffer.load.format(<4 x i32>, i32, i32, i32, i32, i1, i1) #2

attributes #0 = { "ShaderType"="0" }
attributes #1 = { nounwind }
attributes #2 = { nounwind readonly }
