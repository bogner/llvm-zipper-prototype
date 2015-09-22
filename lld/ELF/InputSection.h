//===- InputSection.h -------------------------------------------*- C++ -*-===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_ELF_INPUT_SECTION_H
#define LLD_ELF_INPUT_SECTION_H

#include "lld/Core/LLVM.h"
#include "llvm/Object/ELF.h"

namespace lld {
namespace elf2 {

template <class ELFT> class ObjectFile;
template <class ELFT> class OutputSection;
template <class ELFT> class PltSection;
template <class ELFT> class GotSection;

// A chunk corresponding a section of an input file.
template <class ELFT> class InputSection {
  typedef typename llvm::object::ELFFile<ELFT>::Elf_Shdr Elf_Shdr;
  typedef typename llvm::object::ELFFile<ELFT>::Elf_Rela Elf_Rela;
  typedef typename llvm::object::ELFFile<ELFT>::Elf_Rel Elf_Rel;
  typedef typename llvm::object::ELFFile<ELFT>::Elf_Sym Elf_Sym;
  typedef typename llvm::object::ELFFile<ELFT>::uintX_t uintX_t;

public:
  InputSection(ObjectFile<ELFT> *F, const Elf_Shdr *Header);

  // Returns the size of this chunk (even if this is a common or BSS.)
  size_t getSize() const { return Header->sh_size; }

  // Write this chunk to a mmap'ed file, assuming Buf is pointing to
  // beginning of the output section.
  void writeTo(uint8_t *Buf, const PltSection<ELFT> &PltSec,
               const GotSection<ELFT> &GotSec);

  StringRef getSectionName() const;
  const Elf_Shdr *getSectionHdr() const { return Header; }
  const ObjectFile<ELFT> *getFile() const { return File; }

  // The writer sets and uses the addresses.
  uintX_t getOutputSectionOff() const { return OutputSectionOff; }
  uintX_t getAlign() {
    // The ELF spec states that a value of 0 means the section has no alignment
    // constraits.
    return std::max<uintX_t>(Header->sh_addralign, 1);
  }
  void setOutputSectionOff(uint64_t V) { OutputSectionOff = V; }

  void setOutputSection(OutputSection<ELFT> *O) { Out = O; }
  OutputSection<ELFT> *getOutputSection() const { return Out; }

  // Relocation sections that refer to this one.
  SmallVector<const Elf_Shdr *, 1> RelocSections;

private:
  void relocateOne(uint8_t *Buf, const Elf_Rela &Rel, uint32_t Type,
                   uintX_t BaseAddr, uintX_t SymVA);
  void relocateOne(uint8_t *Buf, const Elf_Rel &Rel, uint32_t Type,
                   uintX_t BaseAddr, uintX_t SymVA);

  template <bool isRela>
  void relocate(uint8_t *Buf,
                llvm::iterator_range<
                    const llvm::object::Elf_Rel_Impl<ELFT, isRela> *> Rels,
                const ObjectFile<ELFT> &File, uintX_t BaseAddr,
                const PltSection<ELFT> &PltSec, const GotSection<ELFT> &GotSec);

  // The offset from beginning of the output sections this chunk was assigned
  // to. The writer sets a value.
  uint64_t OutputSectionOff = 0;

  // The file this chunk was created from.
  ObjectFile<ELFT> *File;

  OutputSection<ELFT> *Out = nullptr;

  const Elf_Shdr *Header;
};

} // namespace elf2
} // namespace lld

#endif
