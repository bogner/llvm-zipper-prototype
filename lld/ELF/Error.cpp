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
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/raw_ostream.h"
#include <mutex>

#if !defined(_MSC_VER) && !defined(__MINGW32__)
#include <unistd.h>
#endif

using namespace lld::elf;
using namespace llvm;

namespace lld {

uint64_t elf::ErrorCount;
raw_ostream *elf::ErrorOS;
StringRef elf::Argv0;

// The functions defined in this file can be called from multiple threads,
// but outs() or errs() are not thread-safe. We protect them using a mutex.
static std::mutex Mu;

static bool useColor() {
  if (Config->ColorDiagnostics == ColorPolicy::Always)
    return true;
  if (Config->ColorDiagnostics == ColorPolicy::Never)
    return false;
  return ErrorOS == &errs() && sys::Process::StandardErrHasColors();
}

static void print(StringRef S, raw_ostream::Colors C) {
  if (useColor()) {
    ErrorOS->changeColor(raw_ostream::WHITE, /*Bold=*/true);
    *ErrorOS << Argv0 + ": ";
    ErrorOS->changeColor(C, true);
    *ErrorOS << S;
    ErrorOS->resetColor();
  } else {
    *ErrorOS << Argv0 + ": " << S;
  }
}

void elf::log(const Twine &Msg) {
  std::lock_guard<std::mutex> Lock(Mu);
  if (Config->Verbose)
    outs() << Argv0 << ": " << Msg << "\n";
}

void elf::warn(const Twine &Msg) {
  if (Config->FatalWarnings) {
    error(Msg);
    return;
  }
  std::lock_guard<std::mutex> Lock(Mu);
  print("warning: ", raw_ostream::MAGENTA);
  *ErrorOS << Msg << "\n";
}

void elf::error(const Twine &Msg) {
  std::lock_guard<std::mutex> Lock(Mu);

  if (Config->ErrorLimit == 0 || ErrorCount < Config->ErrorLimit) {
    print("error: ", raw_ostream::RED);
    *ErrorOS << Msg << "\n";
  } else if (ErrorCount == Config->ErrorLimit) {
    print("error: ", raw_ostream::RED);
    *ErrorOS << "too many errors emitted, stopping now"
             << " (use -error-limit=0 to see all errors)\n";
    if (Config->ExitEarly)
      exitLld(1);
  }

  ++ErrorCount;
}

void elf::error(std::error_code EC, const Twine &Prefix) {
  error(Prefix + ": " + EC.message());
}

void elf::exitLld(int Val) {
  // Dealloc/destroy ManagedStatic variables before calling
  // _exit(). In a non-LTO build, this is a nop. In an LTO
  // build allows us to get the output of -time-passes.
  llvm_shutdown();

  outs().flush();
  errs().flush();
  _exit(Val);
}

void elf::fatal(const Twine &Msg) {
  std::lock_guard<std::mutex> Lock(Mu);
  print("error: ", raw_ostream::RED);
  *ErrorOS << Msg << "\n";
  exitLld(1);
}

void elf::fatal(std::error_code EC, const Twine &Prefix) {
  fatal(Prefix + ": " + EC.message());
}

} // namespace lld
