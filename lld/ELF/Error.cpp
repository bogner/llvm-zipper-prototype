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
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

namespace lld {
namespace elf {

bool HasError;
raw_ostream *ErrorOS;

void log(const Twine &Msg) {
  if (Config->Verbose)
    outs() << Msg << "\n";
}

void warning(const Twine &Msg) {
  if (Config->FatalWarnings)
    error(Msg);
  else
    errs() << Msg << "\n";
}

void error(const Twine &Msg) {
  *ErrorOS << Msg << "\n";
  HasError = true;
}

void error(std::error_code EC, const Twine &Prefix) {
  if (EC)
    error(Prefix + ": " + EC.message());
}

void fatal(const Twine &Msg) {
  errs() << Msg << "\n";
  exit(1);
}

void fatal(const Twine &Msg, const Twine &Prefix) {
  fatal(Prefix + ": " + Msg);
}

void check(std::error_code EC) {
  if (EC)
    fatal(EC.message());
}

} // namespace elf
} // namespace lld
