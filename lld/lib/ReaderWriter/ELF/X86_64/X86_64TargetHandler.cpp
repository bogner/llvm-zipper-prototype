//===- lib/ReaderWriter/ELF/X86_64/X86_64TargetHandler.cpp ----------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Atoms.h"
#include "X86_64TargetHandler.h"
#include "X86_64LinkingContext.h"

using namespace lld;
using namespace elf;

X86_64TargetHandler::X86_64TargetHandler(X86_64LinkingContext &context)
    : DefaultTargetHandler(context), _gotFile(context),
      _relocationHandler(context), _targetLayout(context) {}

void X86_64TargetHandler::addFiles(InputFiles &f) {
  _gotFile.addAtom(*new (_gotFile._alloc) GLOBAL_OFFSET_TABLEAtom(_gotFile));
  _gotFile.addAtom(*new (_gotFile._alloc) TLSGETADDRAtom(_gotFile));
  if (_context.isDynamic())
    _gotFile.addAtom(*new (_gotFile._alloc) DYNAMICAtom(_gotFile));
  f.appendFile(_gotFile);
}
