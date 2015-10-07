// RUN: llvm-mc -filetype=obj -triple=i686-pc-linux %s -o %t
// RUN: llvm-mc -filetype=obj -triple=i686-unknown-linux %p/Inputs/shared.s -o %t2.o
// RUN: ld.lld2 -shared %t2.o -o %t2.so
// RUN: ld.lld2 %t %t2.so -o %t2
// RUN: llvm-readobj -s %t2 | FileCheck --check-prefix=ADDR %s
// RUN: llvm-objdump -d %t2 | FileCheck %s
// REQUIRES: x86

.global _start
_start:

.section       .R_386_32,"ax",@progbits
.global R_386_32
R_386_32:
  movl $R_386_32 + 1, %edx


.section       .R_386_PC32,"ax",@progbits,unique,1
.global R_386_PC32
R_386_PC32:
  call R_386_PC32_2

.section       .R_386_PC32,"ax",@progbits,unique,2
.zero 4
R_386_PC32_2:
  nop

// CHECK: Disassembly of section .R_386_32:
// CHECK-NEXT: R_386_32:
// CHECK-NEXT:  12000: {{.*}} movl $73729, %edx

// CHECK: Disassembly of section .R_386_PC32:
// CHECK-NEXT: R_386_PC32:
// CHECK-NEXT:   12005:  e8 04 00 00 00  calll 4

// CHECK:      R_386_PC32_2:
// CHECK-NEXT:   1200e:  90  nop

// Create a .got
movl bar@GOT, %eax


// ADDR:      Name: .got
// ADDR-NEXT: Type: SHT_PROGBITS
// ADDR-NEXT: Flags [
// ADDR-NEXT:   SHF_ALLOC
// ADDR-NEXT:   SHF_WRITE
// ADDR-NEXT: ]
// ADDR-NEXT: Address: 0x13050

.section .R_386_GOTPC,"ax",@progbits
R_386_GOTPC:
 movl $_GLOBAL_OFFSET_TABLE_, %eax

// 0x13050 - 0x12014 = 4156

// CHECK:      Disassembly of section .R_386_GOTPC:
// CHECK-NEXT: R_386_GOTPC:
// CHECK-NEXT:   12014:  {{.*}} movl  $4156, %eax

.section .dynamic_reloc, "ax",@progbits
        call bar+4
// CHECK:      Disassembly of section .dynamic_reloc:
// CHECK-NEXT: .dynamic_reloc:
// CHECK-NEXT:   12019:  e8 16 00 00 00 calll 22

.section .R_386_GOT32,"ax",@progbits
.global R_386_GOT32
R_386_GOT32:
        movl zed@GOT, %eax
// This is the second symbol in the got, so the offset is 4.
// CHECK:      Disassembly of section .R_386_GOT32:
// CHECK-NEXT: R_386_GOT32:
// CHECK-NEXT:   1201e:  {{.*}} movl 4, %eax
