# REQUIRES: x86
# RUN: llvm-mc -filetype=obj -triple=x86_64-unknown-linux %s -o %t

# RUN: echo "GROUP(" %t ")" > %t.script
# RUN: ld.lld2 -o %t2 %t.script
# RUN: llvm-readobj %t2 > /dev/null

# RUN: echo "GROUP(" %t.script2 ")" > %t.script1
# RUN: echo "GROUP(" %t ")" > %t.script2
# RUN: ld.lld2 -o %t2 %t.script1
# RUN: llvm-readobj %t2 > /dev/null

# RUN: echo "ENTRY(_label)" > %t.script
# RUN: ld.lld2 -o %t2 %t.script %t
# RUN: llvm-readobj %t2 > /dev/null

# RUN: echo "ENTRY(_wrong_label)" > %t.script
# RUN: not lld -flavor gnu2 -o %t2 %t.script %t > %t.log 2>&1
# RUN: FileCheck -check-prefix=ERR-ENTRY %s < %t.log

# ERR-ENTRY: undefined symbol: _wrong_label

# -e has precedence over linker script's ENTRY.
# RUN: echo "ENTRY(_label)" > %t.script
# RUN: ld.lld2 -e _start -o %t2 %t.script %t
# RUN: llvm-readobj -file-headers -symbols %t2 | \
# RUN:   FileCheck -check-prefix=ENTRY-OVERLOAD %s

# ENTRY-OVERLOAD: Entry: [[ENTRY:0x[0-9A-F]+]]
# ENTRY-OVERLOAD: Name: _start
# ENTRY-OVERLOAD-NEXT: Value: [[ENTRY]]

# RUN: echo "OUTPUT_FORMAT(\"elf64-x86-64\") /*/*/ GROUP(" %t ")" > %t.script
# RUN: ld.lld2 -o %t2 %t.script
# RUN: llvm-readobj %t2 > /dev/null

# RUN: echo "GROUP(AS_NEEDED(" %t "))" > %t.script
# RUN: ld.lld2 -o %t2 %t.script
# RUN: llvm-readobj %t2 > /dev/null

# RUN: rm -f %t.out
# RUN: echo "OUTPUT(" %t.out ")" > %t.script
# RUN: ld.lld2 %t.script %t
# RUN: llvm-readobj %t.out > /dev/null

# RUN: echo "SEARCH_DIR(/lib/foo/blah)" > %t.script
# RUN: ld.lld2 %t.script %t
# RUN: llvm-readobj %t.out > /dev/null

# RUN: echo "FOO(BAR)" > %t.script
# RUN: not lld -flavor gnu2 -o foo %t.script > %t.log 2>&1
# RUN: FileCheck -check-prefix=ERR1 %s < %t.log

# ERR1: unknown directive: FOO

.globl _start, _label;
_start:
  mov $60, %rax
  mov $42, %rdi
_label:
  syscall
