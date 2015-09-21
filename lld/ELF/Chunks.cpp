//===- Chunks.cpp ---------------------------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Chunks.h"
#include "Error.h"
#include "InputFiles.h"

using namespace llvm;
using namespace llvm::ELF;

using namespace lld;
using namespace lld::elf2;

template <class ELFT>
InputSection<ELFT>::InputSection(ObjectFile<ELFT> *F, const Elf_Shdr *Header)
    : File(F), Header(Header) {}

template <class ELFT> void InputSection<ELFT>::writeTo(uint8_t *Buf) {
  if (Header->sh_type == SHT_NOBITS)
    return;
  // Copy section contents from source object file to output file.
  ArrayRef<uint8_t> Data = *File->getObj()->getSectionContents(Header);
  memcpy(Buf + OutputSectionOff, Data.data(), Data.size());
}

template <class ELFT> StringRef InputSection<ELFT>::getSectionName() const {
  ErrorOr<StringRef> Name = File->getObj()->getSectionName(Header);
  error(Name);
  return *Name;
}

namespace lld {
namespace elf2 {
template class InputSection<object::ELF32LE>;
template class InputSection<object::ELF32BE>;
template class InputSection<object::ELF64LE>;
template class InputSection<object::ELF64BE>;
}
}
