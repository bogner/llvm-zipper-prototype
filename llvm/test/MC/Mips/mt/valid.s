# RUN: llvm-mc -arch=mips -mcpu=mips32r2 -mattr=+mt -show-encoding < %s \
# RUN:   | FileCheck %s
  dmt           # CHECK:  dmt         # encoding: [0x41,0x60,0x0b,0xc1]
  dmt $5        # CHECK:  dmt $5      # encoding: [0x41,0x65,0x0b,0xc1]
  emt           # CHECK:  emt         # encoding: [0x41,0x60,0x0b,0xe1]
  emt $4        # CHECK:  emt $4      # encoding: [0x41,0x64,0x0b,0xe1]
  dvpe          # CHECK:  dvpe        # encoding: [0x41,0x60,0x00,0x01]
  dvpe $6       # CHECK:  dvpe  $6    # encoding: [0x41,0x66,0x00,0x01]
  evpe          # CHECK:  evpe        # encoding: [0x41,0x60,0x00,0x21]
  evpe $4       # CHECK:  evpe  $4    # encoding: [0x41,0x64,0x00,0x21]

