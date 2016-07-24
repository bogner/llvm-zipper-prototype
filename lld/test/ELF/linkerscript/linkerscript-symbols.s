# REQUIRES: x86
# RUN: llvm-mc -filetype=obj -triple=x86_64-unknown-linux %s -o %t

# Simple symbol assignment. Should raise conflict in case we 
# have duplicates in any input section, but currently simply
# replaces the value.
# RUN: echo "SECTIONS {.text : {*(.text.*)} text_end = .;}" > %t.script
# RUN: ld.lld -o %t1 --script %t.script %t
# RUN: llvm-objdump -t %t1 | FileCheck --check-prefix=SIMPLE %s
# SIMPLE: 0000000000000121         *ABS*    00000000 text_end

# Provide new symbol. The value should be 1, like set in PROVIDE()
# RUN: echo "SECTIONS { PROVIDE(newsym = 1);}" > %t.script
# RUN: ld.lld -o %t1 --script %t.script %t
# RUN: llvm-objdump -t %t1 | FileCheck --check-prefix=PROVIDE1 %s
# PROVIDE1: 0000000000000001         *ABS*    00000000 newsym

# Provide new symbol (hidden). The value should be 1
# RUN: echo "SECTIONS { PROVIDE_HIDDEN(newsym = 1);}" > %t.script
# RUN: ld.lld -o %t1 --script %t.script %t
# RUN: llvm-objdump -t %t1 | FileCheck --check-prefix=HIDDEN1 %s
# HIDDEN1: 0000000000000001         *ABS*    00000000 .hidden newsym

# Provide existing symbol. The value should be 0, even though we 
# have value of 1 in PROVIDE()
# RUN: echo "SECTIONS { PROVIDE(somesym = 1);}" > %t.script
# RUN: ld.lld -o %t1 --script %t.script %t
# RUN: llvm-objdump -t %t1 | FileCheck --check-prefix=PROVIDE2 %s
# PROVIDE2: 0000000000000000         *ABS*    00000000 somesym

# Provide existing symbol. The value should be 0, even though we 
# have value of 1 in PROVIDE(). Visibility should not change
# RUN: echo "SECTIONS { PROVIDE(somesym = 1);}" > %t.script
# RUN: ld.lld -o %t1 --script %t.script %t
# RUN: llvm-objdump -t %t1 | FileCheck --check-prefix=HIDDEN2 %s
# HIDDEN2: 0000000000000000         *ABS*    00000000 somesym

.global _start
_start:
 nop

.global somesym
somesym = 0
