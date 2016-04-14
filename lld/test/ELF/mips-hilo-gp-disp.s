# Check R_MIPS_HI16 / LO16 relocations calculation against _gp_disp.

# RUN: llvm-mc -filetype=obj -triple=mips-unknown-linux %s -o %t1.o
# RUN: llvm-mc -filetype=obj -triple=mips-unknown-linux \
# RUN:         %S/Inputs/mips-dynamic.s -o %t2.o
# RUN: ld.lld %t1.o %t2.o -o %t.exe
# RUN: llvm-objdump -d -t %t.exe | FileCheck -check-prefix=EXE %s
# RUN: ld.lld %t1.o %t2.o -shared -o %t.so
# RUN: llvm-objdump -d -t %t.so | FileCheck -check-prefix=SO %s

# REQUIRES: mips

  .text
  .globl  __start
__start:
  lui    $t0,%hi(_gp_disp)
  addi   $t0,$t0,%lo(_gp_disp)
  lw     $v0,%call16(_foo)($gp)
bar:
  lui    $t0,%hi(_gp_disp)
  addi   $t0,$t0,%lo(_gp_disp)

# EXE:      Disassembly of section .text:
# EXE-NEXT: __start:
# EXE-NEXT:  20000:   3c 08 00 01   lui    $8, 1
#                                              ^-- %hi(0x37ff0-0x20000)
# EXE-NEXT:  20004:   21 08 7f f0   addi   $8, $8, 32752
#                                                  ^-- %lo(0x37ff0-0x20004+4)
# EXE:      bar:
# EXE-NEXT:  2000c:   3c 08 00 01   lui    $8, 1
#                                              ^-- %hi(0x37ff0-0x2000c)
# EXE-NEXT:  20010:   21 08 7f e4   addi   $8, $8, 32740
#                                                  ^-- %lo(0x37ff0-0x20010+4)

# EXE: SYMBOL TABLE:
# EXE: 0002000c     .text   00000000 bar
# EXE: 00037ff0     .got    00000000 .hidden _gp
# EXE: 00020000     .text   00000000 __start

# SO:      Disassembly of section .text:
# SO-NEXT: __start:
# SO-NEXT:  10000:   3c 08 00 01   lui    $8, 1
#                                             ^-- %hi(0x27ff0-0x10000)
# SO-NEXT:  10004:   21 08 7f f0   addi   $8, $8, 32752
#                                                 ^-- %lo(0x27ff0-0x10004+4)
# SO:       bar:
# SO-NEXT:   1000c:   3c 08 00 01   lui    $8, 1
#                                              ^-- %hi(0x27ff0-0x1000c)
# SO-NEXT:   10010:   21 08 7f e4   addi   $8, $8, 32740
#                                                  ^-- %lo(0x27ff0-0x10010+4)

# SO: SYMBOL TABLE:
# SO: 0001000c     .text   00000000 bar
# SO: 00027ff0     .got    00000000 .hidden _gp
# SO: 00010000     .text   00000000 __start
