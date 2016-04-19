// RUN: ld.lld %p/Inputs/gotpcrelx.o -o %t.so -shared
// RUN: llvm-readobj -s -r %t.so | FileCheck %s

// CHECK:      Name: .got
// CHECK-NEXT: Type: SHT_PROGBITS
// CHECK-NEXT: Flags [
// CHECK-NEXT:   SHF_ALLOC
// CHECK-NEXT:   SHF_WRITE
// CHECK-NEXT: ]
// CHECK-NEXT: Address: 0x2090

// CHECK:      Relocations [
// CHECK-NEXT:   Section ({{.*}}) .rela.dyn {
// CHECK-NEXT:     0x2090 R_X86_64_GLOB_DAT foo 0x0
// CHECK-NEXT:     0x2098 R_X86_64_GLOB_DAT bar 0x0
// CHECK-NEXT:   }
// CHECK-NEXT: ]
