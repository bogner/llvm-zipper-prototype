# RUN: llvm-mc -filetype=obj -triple=x86_64-unknown-linux %s -o %tx64
# RUN: ld.lld2 -m elf_x86_64 %tx64 -o %t2x64
# RUN: llvm-readobj -file-headers %t2x64 | FileCheck --check-prefix=X86-64 %s
# RUN: ld.lld2 %tx64 -o %t3x64
# RUN: llvm-readobj -file-headers %t3x64 | FileCheck --check-prefix=X86-64 %s
# X86-64:      ElfHeader {
# X86-64-NEXT:   Ident {
# X86-64-NEXT:     Magic: (7F 45 4C 46)
# X86-64-NEXT:     Class: 64-bit (0x2)
# X86-64-NEXT:     DataEncoding: LittleEndian (0x1)
# X86-64-NEXT:     FileVersion: 1
# X86-64-NEXT:     OS/ABI: SystemV (0x0)
# X86-64-NEXT:     ABIVersion: 0
# X86-64-NEXT:     Unused: (00 00 00 00 00 00 00)
# X86-64-NEXT:   }
# X86-64-NEXT:   Type: Executable (0x2)
# X86-64-NEXT:   Machine: EM_X86_64 (0x3E)
# X86-64-NEXT:   Version: 1
# X86-64-NEXT:   Entry: 0x11000
# X86-64-NEXT:   ProgramHeaderOffset: 0x40
# X86-64-NEXT:   SectionHeaderOffset: 0x1060
# X86-64-NEXT:   Flags [ (0x0)
# X86-64-NEXT:   ]
# X86-64-NEXT:   HeaderSize: 64
# X86-64-NEXT:   ProgramHeaderEntrySize: 56
# X86-64-NEXT:   ProgramHeaderCount: 1
# X86-64-NEXT:   SectionHeaderEntrySize: 64
# X86-64-NEXT:   SectionHeaderCount: 6
# X86-64-NEXT:    StringTableSectionIndex: 5
# X86-64-NEXT: }

# RUN: llvm-mc -filetype=obj -triple=i686-unknown-linux %s -o %tx86
# RUN: ld.lld2 -m elf_i386 %tx86 -o %t2x86
# RUN: llvm-readobj -file-headers %t2x86 | FileCheck --check-prefix=X86 %s
# RUN: ld.lld2 %tx86 -o %t3x86
# RUN: llvm-readobj -file-headers %t3x86 | FileCheck --check-prefix=X86 %s
# X86:      ElfHeader {
# X86-NEXT:   Ident {
# X86-NEXT:     Magic: (7F 45 4C 46)
# X86-NEXT:     Class: 32-bit (0x1)
# X86-NEXT:     DataEncoding: LittleEndian (0x1)
# X86-NEXT:     FileVersion: 1
# X86-NEXT:     OS/ABI: SystemV (0x0)
# X86-NEXT:     ABIVersion: 0
# X86-NEXT:     Unused: (00 00 00 00 00 00 00)
# X86-NEXT:   }
# X86-NEXT:   Type: Executable (0x2)
# X86-NEXT:   Machine: EM_386 (0x3)
# X86-NEXT:   Version: 1
# X86-NEXT:   Entry: 0x11000
# X86-NEXT:   ProgramHeaderOffset: 0x34
# X86-NEXT:   SectionHeaderOffset: 0x104C
# X86-NEXT:   Flags [ (0x0)
# X86-NEXT:   ]
# X86-NEXT:   HeaderSize: 52
# X86-NEXT:   ProgramHeaderEntrySize: 32
# X86-NEXT:   ProgramHeaderCount: 1
# X86-NEXT:   SectionHeaderEntrySize: 40
# X86-NEXT:   SectionHeaderCount: 6
# X86-NEXT:    StringTableSectionIndex: 5
# X86-NEXT: }

# RUN: llvm-mc -filetype=obj -triple=powerpc64-unknown-linux %s -o %tppc64
# RUN: ld.lld2 -m elf64ppc %tppc64 -o %t2ppc64
# RUN: llvm-readobj -file-headers %t2ppc64 | FileCheck --check-prefix=PPC64 %s
# RUN: ld.lld2 %tppc64 -o %t3ppc64
# RUN: llvm-readobj -file-headers %t3ppc64 | FileCheck --check-prefix=PPC64 %s
# PPC64:      ElfHeader {
# PPC64-NEXT:   Ident {
# PPC64-NEXT:     Magic: (7F 45 4C 46)
# PPC64-NEXT:     Class: 64-bit (0x2)
# PPC64-NEXT:     DataEncoding: BigEndian (0x2)
# PPC64-NEXT:     FileVersion: 1
# PPC64-NEXT:     OS/ABI: SystemV (0x0)
# PPC64-NEXT:     ABIVersion: 0
# PPC64-NEXT:     Unused: (00 00 00 00 00 00 00)
# PPC64-NEXT:   }
# PPC64-NEXT:   Type: Executable (0x2)
# PPC64-NEXT:   Machine: EM_PPC64 (0x15)
# PPC64-NEXT:   Version: 1
# PPC64-NEXT:   Entry: 0x10001000
# PPC64-NEXT:   ProgramHeaderOffset: 0x40
# PPC64-NEXT:   SectionHeaderOffset: 0x1060
# PPC64-NEXT:   Flags [ (0x0)
# PPC64-NEXT:   ]
# PPC64-NEXT:   HeaderSize: 64
# PPC64-NEXT:   ProgramHeaderEntrySize: 56
# PPC64-NEXT:   ProgramHeaderCount: 1
# PPC64-NEXT:   SectionHeaderEntrySize: 64
# PPC64-NEXT:   SectionHeaderCount: 6
# PPC64-NEXT:    StringTableSectionIndex: 5
# PPC64-NEXT: }

# RUN: llvm-mc -filetype=obj -triple=powerpc-unknown-linux %s -o %tppc
# RUN: ld.lld2 -m elf32ppc %tppc -o %t2ppc
# RUN: llvm-readobj -file-headers %t2ppc | FileCheck --check-prefix=PPC %s
# RUN: ld.lld2 %tppc -o %t3ppc
# RUN: llvm-readobj -file-headers %t3ppc | FileCheck --check-prefix=PPC %s
# PPC:      ElfHeader {
# PPC-NEXT:   Ident {
# PPC-NEXT:     Magic: (7F 45 4C 46)
# PPC-NEXT:     Class: 32-bit (0x1)
# PPC-NEXT:     DataEncoding: BigEndian (0x2)
# PPC-NEXT:     FileVersion: 1
# PPC-NEXT:     OS/ABI: SystemV (0x0)
# PPC-NEXT:     ABIVersion: 0
# PPC-NEXT:     Unused: (00 00 00 00 00 00 00)
# PPC-NEXT:   }
# PPC-NEXT:   Type: Executable (0x2)
# PPC-NEXT:   Machine: EM_PPC (0x14)
# PPC-NEXT:   Version: 1
# PPC-NEXT:   Entry: 0x10001000
# PPC-NEXT:   ProgramHeaderOffset: 0x34
# PPC-NEXT:   SectionHeaderOffset: 0x104C
# PPC-NEXT:   Flags [ (0x0)
# PPC-NEXT:   ]
# PPC-NEXT:   HeaderSize: 52
# PPC-NEXT:   ProgramHeaderEntrySize: 32
# PPC-NEXT:   ProgramHeaderCount: 1
# PPC-NEXT:   SectionHeaderEntrySize: 40
# PPC-NEXT:   SectionHeaderCount: 6
# PPC-NEXT:    StringTableSectionIndex: 5
# PPC-NEXT: }

# RUN: llvm-mc -filetype=obj -triple=mips-unknown-linux %s -o %tmips
# RUN: lld -flavor gnu2 -m elf32btsmip -e _start %tmips -o %t2mips
# RUN: llvm-readobj -file-headers %t2mips | FileCheck --check-prefix=MIPS %s
# RUN: lld -flavor gnu2 %tmips -e _start -o %t3mips
# RUN: llvm-readobj -file-headers %t3mips | FileCheck --check-prefix=MIPS %s
# MIPS:      ElfHeader {
# MIPS-NEXT:   Ident {
# MIPS-NEXT:     Magic: (7F 45 4C 46)
# MIPS-NEXT:     Class: 32-bit (0x1)
# MIPS-NEXT:     DataEncoding: BigEndian (0x2)
# MIPS-NEXT:     FileVersion: 1
# MIPS-NEXT:     OS/ABI: SystemV (0x0)
# MIPS-NEXT:     ABIVersion: 0
# MIPS-NEXT:     Unused: (00 00 00 00 00 00 00)
# MIPS-NEXT:   }
# MIPS-NEXT:   Type: Executable (0x2)
# MIPS-NEXT:   Machine: EM_MIPS (0x8)
# MIPS-NEXT:   Version: 1
# MIPS-NEXT:   Entry: 0x11030
# MIPS-NEXT:   ProgramHeaderOffset: 0x34
# MIPS-NEXT:   SectionHeaderOffset: 0x1094
# MIPS-NEXT:   Flags [ (0x0)
# MIPS-NEXT:   ]

# RUN: llvm-mc -filetype=obj -triple=mipsel-unknown-linux %s -o %tmipsel
# RUN: lld -flavor gnu2 -m elf32ltsmip -e _start %tmipsel -o %t2mipsel
# RUN: llvm-readobj -file-headers %t2mipsel | FileCheck --check-prefix=MIPSEL %s
# RUN: lld -flavor gnu2 %tmipsel -e _start -o %t3mipsel
# RUN: llvm-readobj -file-headers %t3mipsel | FileCheck --check-prefix=MIPSEL %s
# MIPSEL:      ElfHeader {
# MIPSEL-NEXT:   Ident {
# MIPSEL-NEXT:     Magic: (7F 45 4C 46)
# MIPSEL-NEXT:     Class: 32-bit (0x1)
# MIPSEL-NEXT:     DataEncoding: LittleEndian (0x1)
# MIPSEL-NEXT:     FileVersion: 1
# MIPSEL-NEXT:     OS/ABI: SystemV (0x0)
# MIPSEL-NEXT:     ABIVersion: 0
# MIPSEL-NEXT:     Unused: (00 00 00 00 00 00 00)
# MIPSEL-NEXT:   }
# MIPSEL-NEXT:   Type: Executable (0x2)
# MIPSEL-NEXT:   Machine: EM_MIPS (0x8)
# MIPSEL-NEXT:   Version: 1
# MIPSEL-NEXT:   Entry: 0x11030
# MIPSEL-NEXT:   ProgramHeaderOffset: 0x34
# MIPSEL-NEXT:   SectionHeaderOffset: 0x1094
# MIPSEL-NEXT:   Flags [ (0x0)
# MIPSEL-NEXT:   ]

# REQUIRES: x86,ppc,mips

.globl _start;
_start:
