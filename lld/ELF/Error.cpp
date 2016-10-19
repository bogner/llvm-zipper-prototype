//===- Error.cpp ----------------------------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Error.h"
#include "Config.h"

#include "llvm/ADT/Twine.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

namespace lld {

bool elf::HasError;
raw_ostream *elf::ErrorOS;

void elf::log(const Twine &Msg) {
  if (Config->Verbose)
    outs() << Msg << "\n";
}

void elf::warn(const Twine &Msg) {
  if (Config->FatalWarnings)
    error(Msg);
  else
    *ErrorOS << "warning: " << Msg << "\n";
}

void elf::error(const Twine &Msg) {
  *ErrorOS << "error: " << Msg << "\n";
  HasError = true;
}

void elf::error(std::error_code EC, const Twine &Prefix) {
  error(Prefix + ": " + EC.message());
}

void elf::fatal(const Twine &Msg) {
  *ErrorOS << "error: " << Msg << "\n";
  exit(1);
}

void elf::fatal(std::error_code EC, const Twine &Prefix) {
  fatal(Prefix + ": " + EC.message());
}

} // namespace lld
