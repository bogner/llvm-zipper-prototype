//===- Symbols.cpp --------------------------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Symbols.h"

#include "Config.h"
#include "InputChunks.h"
#include "InputFiles.h"
#include "lld/Common/ErrorHandler.h"
#include "lld/Common/Strings.h"

#define DEBUG_TYPE "lld"

using namespace llvm;
using namespace llvm::wasm;
using namespace lld;
using namespace lld::wasm;

DefinedFunction *WasmSym::CallCtors;
DefinedGlobal *WasmSym::DsoHandle;
DefinedGlobal *WasmSym::DataEnd;
DefinedGlobal *WasmSym::HeapBase;
DefinedGlobal *WasmSym::StackPointer;

bool Symbol::hasOutputIndex() const {
  if (auto *F = dyn_cast_or_null<InputFunction>(Chunk))
    return F->hasOutputIndex();
  return OutputIndex != INVALID_INDEX;
}

uint32_t Symbol::getOutputIndex() const {
  if (auto *F = dyn_cast_or_null<InputFunction>(Chunk))
    return F->getOutputIndex();
  assert(OutputIndex != INVALID_INDEX);
  return OutputIndex;
}

void Symbol::setOutputIndex(uint32_t Index) {
  DEBUG(dbgs() << "setOutputIndex " << Name << " -> " << Index << "\n");
  assert(!dyn_cast_or_null<InputFunction>(Chunk));
  assert(OutputIndex == INVALID_INDEX);
  OutputIndex = Index;
}

bool Symbol::isWeak() const {
  return (Flags & WASM_SYMBOL_BINDING_MASK) == WASM_SYMBOL_BINDING_WEAK;
}

bool Symbol::isLocal() const {
  return (Flags & WASM_SYMBOL_BINDING_MASK) == WASM_SYMBOL_BINDING_LOCAL;
}

bool Symbol::isHidden() const {
  return (Flags & WASM_SYMBOL_VISIBILITY_MASK) == WASM_SYMBOL_VISIBILITY_HIDDEN;
}

void Symbol::setHidden(bool IsHidden) {
  DEBUG(dbgs() << "setHidden: " << Name << " -> " << IsHidden << "\n");
  Flags &= ~WASM_SYMBOL_VISIBILITY_MASK;
  if (IsHidden)
    Flags |= WASM_SYMBOL_VISIBILITY_HIDDEN;
  else
    Flags |= WASM_SYMBOL_VISIBILITY_DEFAULT;
}

FunctionSymbol::FunctionSymbol(StringRef Name, Kind K, uint32_t Flags,
                               InputFile *F, InputFunction *Function)
    : Symbol(Name, K, Flags, F, Function), FunctionType(&Function->Signature) {}

uint32_t FunctionSymbol::getTableIndex() const {
  if (auto *F = dyn_cast_or_null<InputFunction>(Chunk))
    return F->getTableIndex();
  assert(TableIndex != INVALID_INDEX);
  return TableIndex;
}

bool FunctionSymbol::hasTableIndex() const {
  if (auto *F = dyn_cast_or_null<InputFunction>(Chunk))
    return F->hasTableIndex();
  return TableIndex != INVALID_INDEX;
}

void FunctionSymbol::setTableIndex(uint32_t Index) {
  // For imports, we set the table index here on the Symbol; for defined
  // functions we set the index on the InputFunction so that we don't export
  // the same thing twice (keeps the table size down).
  if (auto *F = dyn_cast_or_null<InputFunction>(Chunk)) {
    F->setTableIndex(Index);
    return;
  }
  DEBUG(dbgs() << "setTableIndex " << Name << " -> " << Index << "\n");
  assert(TableIndex == INVALID_INDEX);
  TableIndex = Index;
}

uint32_t DefinedGlobal::getVirtualAddress() const {
  assert(isGlobal());
  DEBUG(dbgs() << "getVirtualAddress: " << getName() << "\n");
  return Chunk ? dyn_cast<InputSegment>(Chunk)->translateVA(VirtualAddress)
               : VirtualAddress;
}

void DefinedGlobal::setVirtualAddress(uint32_t Value) {
  DEBUG(dbgs() << "setVirtualAddress " << Name << " -> " << Value << "\n");
  assert(isGlobal());
  VirtualAddress = Value;
}

std::string lld::toString(const wasm::Symbol &Sym) {
  if (Config->Demangle)
    if (Optional<std::string> S = demangleItanium(Sym.getName()))
      return "`" + *S + "'";
  return Sym.getName();
}

std::string lld::toString(wasm::Symbol::Kind Kind) {
  switch (Kind) {
  case wasm::Symbol::DefinedFunctionKind:
    return "DefinedFunction";
  case wasm::Symbol::DefinedGlobalKind:
    return "DefinedGlobal";
  case wasm::Symbol::UndefinedFunctionKind:
    return "UndefinedFunction";
  case wasm::Symbol::UndefinedGlobalKind:
    return "UndefinedGlobal";
  case wasm::Symbol::LazyKind:
    return "LazyKind";
  }
  llvm_unreachable("Invalid symbol kind!");
}
