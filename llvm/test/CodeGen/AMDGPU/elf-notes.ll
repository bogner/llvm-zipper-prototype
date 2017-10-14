; RUN: llc -mtriple=amdgcn-amd-unknown -mcpu=gfx800 -mattr=+code-object-v3 < %s | FileCheck --check-prefix=GCN --check-prefix=OSABI-UNK --check-prefix=GFX800 %s
; RUN: llc -mtriple=amdgcn-amd-unknown -mcpu=iceland -mattr=+code-object-v3 < %s | FileCheck --check-prefix=GCN --check-prefix=OSABI-UNK --check-prefix=GFX800 %s
; RUN: llc -mtriple=amdgcn-amd-amdhsa -mcpu=gfx800 -mattr=+code-object-v3 < %s | FileCheck --check-prefix=GCN --check-prefix=OSABI-HSA --check-prefix=GFX800 %s
; RUN: llc -mtriple=amdgcn-amd-amdhsa -mcpu=iceland -mattr=+code-object-v3 < %s | FileCheck --check-prefix=GCN --check-prefix=OSABI-HSA --check-prefix=GFX800 %s
; RUN: llc -mtriple=amdgcn-amd-amdpal -mcpu=gfx800 -mattr=+code-object-v3 < %s | FileCheck --check-prefix=GCN --check-prefix=OSABI-PAL --check-prefix=GFX800 %s
; RUN: llc -mtriple=amdgcn-amd-amdpal -mcpu=iceland -mattr=+code-object-v3 < %s | FileCheck --check-prefix=GCN --check-prefix=OSABI-PAL --check-prefix=GFX800 %s
; RUN: llc -march=r600 -mattr=+code-object-v3 < %s | FileCheck --check-prefix=R600 %s

; OSABI-UNK-NOT: .hsa_code_object_version
; OSABI-UNK-NOT: .hsa_code_object_isa
; OSABI-UNK: .amd_amdgpu_isa "amdgcn-amd-unknown--gfx800"
; OSABI-UNK-NOT: .amd_amdgpu_hsa_metadata
; OSABI-UNK-NOT: .amd_amdgpu_pal_metadata

; OSABI-HSA-NOT: .hsa_code_object_version
; OSABI-HSA-NOT: .hsa_code_object_isa
; OSABI-HSA: .amd_amdgpu_isa "amdgcn-amd-amdhsa--gfx800"
; OSABI-HSA: .amd_amdgpu_hsa_metadata
; OSABI-HSA-NOT: .amd_amdgpu_pal_metadata

; OSABI-PAL-NOT: .hsa_code_object_version
; OSABI-PAL-NOT: .hsa_code_object_isa
; OSABI-PAL: .amd_amdgpu_isa "amdgcn-amd-amdpal--gfx800"
; OSABI-PAL-NOT: .amd_amdgpu_hsa_metadata
; OSABI-PAL: .amd_amdgpu_pal_metadata

; RUN: llc -mtriple=amdgcn-amd-unknown -mcpu=gfx800 -mattr=+code-object-v3 -filetype=obj < %s | llvm-readobj -elf-output-style=GNU -notes  | FileCheck --check-prefix=GCN --check-prefix=OSABI-UNK-ELF --check-prefix=GFX800 %s
; RUN: llc -mtriple=amdgcn-amd-amdhsa -mcpu=gfx800 -mattr=+code-object-v3 -filetype=obj < %s | llvm-readobj -elf-output-style=GNU -notes  | FileCheck --check-prefix=GCN --check-prefix=OSABI-HSA-ELF --check-prefix=GFX800 %s
; RUN: llc -mtriple=amdgcn-amd-amdpal -mcpu=gfx800 -mattr=+code-object-v3 -filetype=obj < %s | llvm-readobj -elf-output-style=GNU -notes  | FileCheck --check-prefix=GCN --check-prefix=OSABI-PAL-ELF --check-prefix=GFX800 %s

; R600-NOT: .hsa_code_object_version
; R600-NOT: .hsa_code_object_isa
; R600-NOT: .amd_amdgpu_isa
; R600-NOT: .amd_amdgpu_hsa_metadata
; R600-NOT: .amd_amdgpu_pal_metadatas

define amdgpu_kernel void @elf_notes() {
  ret void
}

; OSABI-UNK-ELF-NOT: Unknown note type
; OSABI-UNK-ELF: NT_AMD_AMDGPU_ISA (ISA Version)
; OSABI-UNK-ELF-NOT: Unknown note type
; OSABI-UNK-ELF-NOT: NT_AMD_AMDGPU_HSA_METADATA (HSA Metadata)
; OSABI-UNK-ELF-NOT: Unknown note type
; OSABI-UNK-ELF-NOT: NT_AMD_AMDGPU_PAL_METADATA (PAL Metadata)
; OSABI-UNK-ELF-NOT: Unknown note type

; OSABI-HSA-ELF-NOT: Unknown note type
; OSABI-HSA-ELF: NT_AMD_AMDGPU_ISA (ISA Version)
; OSABI-HSA-ELF-NOT: Unknown note type
; OSABI-HSA-ELF: NT_AMD_AMDGPU_HSA_METADATA (HSA Metadata)
; OSABI-HSA-ELF-NOT: Unknown note type
; OSABI-HSA-ELF-NOT: NT_AMD_AMDGPU_PAL_METADATA (PAL Metadata)
; OSABI-HSA-ELF-NOT: Unknown note type

; OSABI-PAL-ELF-NOT: Unknown note type
; OSABI-PAL-ELF: NT_AMD_AMDGPU_ISA (ISA Version)
; OSABI-PAL-ELF-NOT: Unknown note type
; OSABI-PAL-ELF-NOT: NT_AMD_AMDGPU_HSA_METADATA (HSA Metadata)
; OSABI-PAL-ELF-NOT: Unknown note type
; OSABI-PAL-ELF: NT_AMD_AMDGPU_PAL_METADATA (PAL Metadata)
; OSABI-PAL-ELF-NOT: Unknown note type
