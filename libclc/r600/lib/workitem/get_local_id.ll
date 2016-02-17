declare i32 @llvm.r600.read.tidig.x() #0
declare i32 @llvm.r600.read.tidig.y() #0
declare i32 @llvm.r600.read.tidig.z() #0

define i32 @get_local_id(i32 %dim) #1 {
  switch i32 %dim, label %default [
    i32 0, label %x_dim
    i32 1, label %y_dim
    i32 2, label %z_dim
  ]

x_dim:
  %x = tail call i32 @llvm.r600.read.tidig.x(), !range !0
  ret i32 %x

y_dim:
  %y = tail call i32 @llvm.r600.read.tidig.y(), !range !0
  ret i32 %y
z_dim:

  %z = tail call i32 @llvm.r600.read.tidig.z(), !range !0
  ret i32 %z

default:
  ret i32 0
}

attributes #0 = { nounwind readnone }
attributes #1 = { alwaysinline norecurse nounwind readnone }

!0 = !{ i32 0, i32 2048 }
