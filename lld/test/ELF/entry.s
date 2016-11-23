# RUN: llvm-mc -filetype=obj -triple=x86_64-unknown-linux %s -o %t1

# RUN: ld.lld -e foobar %t1 -o %t2 2>&1 | FileCheck -check-prefix=WARN %s
# RUN: llvm-readobj -file-headers %t2 | FileCheck -check-prefix=NOENTRY %s
# RUN: ld.lld %t1 -o %t2 2>&1 | FileCheck -check-prefix=WARN2 %s

# RUN: ld.lld -shared -e foobar %t1 -o %t2 2>&1 | FileCheck -check-prefix=WARN %s
# RUN: ld.lld -shared --fatal-warnings -e entry %t1 -o %t2
# RUN: ld.lld -shared --fatal-warnings %t1 -o %t2

# RUN: ld.lld %t1 -o %t2 -e entry
# RUN: llvm-readobj -file-headers %t2 | FileCheck -check-prefix=SYM %s
# RUN: ld.lld %t1 --fatal-warnings -shared -o %t2 -e entry
# RUN: llvm-readobj -file-headers %t2 | FileCheck -check-prefix=DSO %s
# RUN: ld.lld %t1 -o %t2 --entry=4096
# RUN: llvm-readobj -file-headers %t2 | FileCheck -check-prefix=DEC %s
# RUN: ld.lld %t1 -o %t2 --entry 0xcafe
# RUN: llvm-readobj -file-headers %t2 | FileCheck -check-prefix=HEX %s
# RUN: ld.lld %t1 -o %t2 -e 0777
# RUN: llvm-readobj -file-headers %t2 | FileCheck -check-prefix=OCT %s

# WARN: warning: entry symbol foobar not found, assuming 0
# WARN2: warning: entry symbol _start not found, assuming 0

# NOENTRY: Entry: 0x0
# SYM: Entry: 0x201000
# DSO: Entry: 0x1000
# DEC: Entry: 0x1000
# HEX: Entry: 0xCAFE
# OCT: Entry: 0x1FF

.globl entry
entry:
