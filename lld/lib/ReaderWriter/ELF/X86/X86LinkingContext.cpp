//===- lib/ReaderWriter/ELF/X86/X86LinkingContext.cpp ---------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "X86LinkingContext.h"

#include "lld/Core/LLVM.h"

#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/ErrorOr.h"

using namespace lld;

#define LLD_CASE(name) .Case(#name, llvm::ELF::name)

ErrorOr<Reference::Kind>
elf::X86LinkingContext::relocKindFromString(StringRef str) const {
  int32_t ret = llvm::StringSwitch<int32_t>(str) LLD_CASE(R_386_NONE)
      LLD_CASE(R_386_PC32).Default(-1);

  if (ret == -1)
    return make_error_code(YamlReaderError::illegal_value);
  return ret;
}

#undef LLD_CASE

#define LLD_CASE(name)                                                         \
  case llvm::ELF::name:                                                        \
  return std::string(#name);

ErrorOr<std::string>
elf::X86LinkingContext::stringFromRelocKind(Reference::Kind kind) const {
  switch (kind) {
    LLD_CASE(R_386_NONE)
    LLD_CASE(R_386_PC32)
  }

  return make_error_code(YamlReaderError::illegal_value);
}
