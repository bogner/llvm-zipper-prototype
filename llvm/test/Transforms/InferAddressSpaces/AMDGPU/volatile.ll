; RUN: opt -S -mtriple=amdgcn-amd-amdhsa -infer-address-spaces %s | FileCheck %s

; Check that volatile users of addrspacecast are not replaced.

; CHECK-LABEL: @volatile_load_flat_from_global(
; CHECK: load volatile i32, i32 addrspace(4)*
; CHECK: store i32 %val, i32 addrspace(1)*
define void @volatile_load_flat_from_global(i32 addrspace(1)* nocapture %input, i32 addrspace(1)* nocapture %output) #0 {
  %tmp0 = addrspacecast i32 addrspace(1)* %input to i32 addrspace(4)*
  %tmp1 = addrspacecast i32 addrspace(1)* %output to i32 addrspace(4)*
  %val = load volatile i32, i32 addrspace(4)* %tmp0, align 4
  store i32 %val, i32 addrspace(4)* %tmp1, align 4
  ret void
}

; CHECK-LABEL: @volatile_load_flat_from_constant(
; CHECK: load volatile i32, i32 addrspace(4)*
; CHECK: store i32 %val, i32 addrspace(1)*
define void @volatile_load_flat_from_constant(i32 addrspace(2)* nocapture %input, i32 addrspace(1)* nocapture %output) #0 {
  %tmp0 = addrspacecast i32 addrspace(2)* %input to i32 addrspace(4)*
  %tmp1 = addrspacecast i32 addrspace(1)* %output to i32 addrspace(4)*
  %val = load volatile i32, i32 addrspace(4)* %tmp0, align 4
  store i32 %val, i32 addrspace(4)* %tmp1, align 4
  ret void
}

; CHECK-LABEL: @volatile_load_flat_from_group(
; CHECK: load volatile i32, i32 addrspace(4)*
; CHECK: store i32 %val, i32 addrspace(3)*
define void @volatile_load_flat_from_group(i32 addrspace(3)* nocapture %input, i32 addrspace(3)* nocapture %output) #0 {
  %tmp0 = addrspacecast i32 addrspace(3)* %input to i32 addrspace(4)*
  %tmp1 = addrspacecast i32 addrspace(3)* %output to i32 addrspace(4)*
  %val = load volatile i32, i32 addrspace(4)* %tmp0, align 4
  store i32 %val, i32 addrspace(4)* %tmp1, align 4
  ret void
}

; CHECK-LABEL: @volatile_load_flat_from_private(
; CHECK: load volatile i32, i32 addrspace(4)*
; CHECK: store i32 %val, i32*
define void @volatile_load_flat_from_private(i32* nocapture %input, i32* nocapture %output) #0 {
  %tmp0 = addrspacecast i32* %input to i32 addrspace(4)*
  %tmp1 = addrspacecast i32* %output to i32 addrspace(4)*
  %val = load volatile i32, i32 addrspace(4)* %tmp0, align 4
  store i32 %val, i32 addrspace(4)* %tmp1, align 4
  ret void
}

; CHECK-LABEL: @volatile_store_flat_to_global(
; CHECK: load i32, i32 addrspace(1)*
; CHECK: store volatile i32 %val, i32 addrspace(4)*
define void @volatile_store_flat_to_global(i32 addrspace(1)* nocapture %input, i32 addrspace(1)* nocapture %output) #0 {
  %tmp0 = addrspacecast i32 addrspace(1)* %input to i32 addrspace(4)*
  %tmp1 = addrspacecast i32 addrspace(1)* %output to i32 addrspace(4)*
  %val = load i32, i32 addrspace(4)* %tmp0, align 4
  store volatile i32 %val, i32 addrspace(4)* %tmp1, align 4
  ret void
}

; CHECK-LABEL: @volatile_store_flat_to_group(
; CHECK: load i32, i32 addrspace(3)*
; CHECK: store volatile i32 %val, i32 addrspace(4)*
define void @volatile_store_flat_to_group(i32 addrspace(3)* nocapture %input, i32 addrspace(3)* nocapture %output) #0 {
  %tmp0 = addrspacecast i32 addrspace(3)* %input to i32 addrspace(4)*
  %tmp1 = addrspacecast i32 addrspace(3)* %output to i32 addrspace(4)*
  %val = load i32, i32 addrspace(4)* %tmp0, align 4
  store volatile i32 %val, i32 addrspace(4)* %tmp1, align 4
  ret void
}

; CHECK-LABEL: @volatile_store_flat_to_private(
; CHECK: load i32, i32*
; CHECK: store volatile i32 %val, i32 addrspace(4)*
define void @volatile_store_flat_to_private(i32* nocapture %input, i32* nocapture %output) #0 {
  %tmp0 = addrspacecast i32* %input to i32 addrspace(4)*
  %tmp1 = addrspacecast i32* %output to i32 addrspace(4)*
  %val = load i32, i32 addrspace(4)* %tmp0, align 4
  store volatile i32 %val, i32 addrspace(4)* %tmp1, align 4
  ret void
}

attributes #0 = { nounwind }