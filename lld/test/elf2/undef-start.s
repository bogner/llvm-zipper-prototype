# RUN: llvm-mc -filetype=obj -triple=x86_64-unknown-linux %s -o %t
# RUN: not lld -flavor gnu2 %t -o %t2 2>&1 | FileCheck %s
# CHECK: undefined symbol: _start
# REQUIRES: x86
