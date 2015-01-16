//===- lib/ReaderWriter/PECOFF/LinkerGeneratedSymbolFile.h ----------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Atoms.h"
#include "lld/Core/ArchiveLibraryFile.h"
#include "lld/Core/Simple.h"
#include "lld/ReaderWriter/PECOFFLinkingContext.h"
#include "llvm/Support/Allocator.h"
#include <algorithm>
#include <mutex>

using llvm::COFF::WindowsSubsystem;

namespace lld {
namespace pecoff {

bool findDecoratedSymbol(PECOFFLinkingContext *ctx,
                         std::string sym, std::string &res);

namespace impl {

/// The defined atom for dllexported symbols with __imp_ prefix.
class ImpPointerAtom : public COFFLinkerInternalAtom {
public:
  ImpPointerAtom(const File &file, StringRef symbolName, uint64_t ordinal)
      : COFFLinkerInternalAtom(file, /*oridnal*/ 0, std::vector<uint8_t>(4),
                               symbolName),
        _ordinal(ordinal) {}

  uint64_t ordinal() const override { return _ordinal; }
  Scope scope() const override { return scopeGlobal; }
  ContentType contentType() const override { return typeData; }
  Alignment alignment() const override { return Alignment(4); }
  ContentPermissions permissions() const override { return permR__; }

private:
  uint64_t _ordinal;
};

class ImpSymbolFile : public SimpleFile {
public:
  ImpSymbolFile(StringRef defsym, StringRef undefsym, uint64_t ordinal,
                bool is64)
      : SimpleFile(defsym), _undefined(*this, undefsym),
        _defined(*this, defsym, ordinal) {
    auto *ref = is64 ? new COFFReference(&_undefined, 0,
                                         llvm::COFF::IMAGE_REL_AMD64_ADDR32,
                                         Reference::KindArch::x86_64)
                     : new COFFReference(&_undefined, 0,
                                         llvm::COFF::IMAGE_REL_I386_DIR32,
                                         Reference::KindArch::x86);
    _defined.addReference(std::unique_ptr<COFFReference>(ref));
    addAtom(_defined);
    addAtom(_undefined);
  };

private:
  SimpleUndefinedAtom _undefined;
  ImpPointerAtom _defined;
};

class VirtualArchiveLibraryFile : public ArchiveLibraryFile {
public:
  VirtualArchiveLibraryFile(StringRef filename)
      : ArchiveLibraryFile(filename) {}

  const atom_collection<DefinedAtom> &defined() const override {
    return _definedAtoms;
  }

  const atom_collection<UndefinedAtom> &undefined() const override {
    return _undefinedAtoms;
  }

  const atom_collection<SharedLibraryAtom> &sharedLibrary() const override {
    return _sharedLibraryAtoms;
  }

  const atom_collection<AbsoluteAtom> &absolute() const override {
    return _absoluteAtoms;
  }

  std::error_code
  parseAllMembers(std::vector<std::unique_ptr<File>> &result) override {
    return std::error_code();
  }

private:
  atom_collection_vector<DefinedAtom> _definedAtoms;
  atom_collection_vector<UndefinedAtom> _undefinedAtoms;
  atom_collection_vector<SharedLibraryAtom> _sharedLibraryAtoms;
  atom_collection_vector<AbsoluteAtom> _absoluteAtoms;
};

// A file to make Resolver to resolve a symbol TO instead of a symbol FROM,
// using fallback mechanism for an undefined symbol. One can virtually rename an
// undefined symbol using this file.
class SymbolRenameFile : public SimpleFile {
public:
  SymbolRenameFile(StringRef from, StringRef to)
      : SimpleFile("<symbol-rename>"), _fromSym(from), _toSym(to),
        _from(*this, _fromSym, &_to), _to(*this, _toSym) {
    addAtom(_from);
  };

private:
  std::string _fromSym;
  std::string _toSym;
  COFFUndefinedAtom _from;
  COFFUndefinedAtom _to;
};

} // namespace impl

// A virtual file containing absolute symbol __ImageBase. __ImageBase (or
// ___ImageBase on x86) is a linker-generated symbol whose address is the same
// as the image base address.
class LinkerGeneratedSymbolFile : public SimpleFile {
public:
  LinkerGeneratedSymbolFile(const PECOFFLinkingContext &ctx)
      : SimpleFile("<linker-internal-file>"),
        _imageBaseAtom(*this, ctx.decorateSymbol("__ImageBase"),
                       Atom::scopeGlobal, ctx.getBaseAddress()) {
    addAtom(_imageBaseAtom);
  };

private:
  COFFAbsoluteAtom _imageBaseAtom;
};

// A LocallyImporteSymbolFile is an archive file containing __imp_
// symbols for local use.
//
// For each defined symbol, linker creates an implicit defined symbol
// by appending "__imp_" prefix to the original name. The content of
// the implicit symbol is a pointer to the original symbol
// content. This feature allows one to compile and link the following
// code without error, although _imp__hello is not defined in the
// code. (the leading "_" in this example is automatically appended,
// assuming it's x86.)
//
//   void hello() { printf("Hello\n"); }
//   extern void (*_imp__hello)();
//   int main() {
//      _imp__hello();
//      return 0;
//   }
//
// This odd feature is for the compatibility with MSVC link.exe.
class LocallyImportedSymbolFile : public impl::VirtualArchiveLibraryFile {
public:
  LocallyImportedSymbolFile(const PECOFFLinkingContext &ctx)
      : VirtualArchiveLibraryFile("__imp_"), _is64(ctx.is64Bit()),
        _ordinal(0) {}

  const File *find(StringRef sym, bool dataSymbolOnly) const override {
    std::string prefix = "__imp_";
    if (!sym.startswith(prefix))
      return nullptr;
    StringRef undef = sym.substr(prefix.size());
    return new (_alloc) impl::ImpSymbolFile(sym, undef, _ordinal++, _is64);
  }

private:
  bool _is64;
  mutable uint64_t _ordinal;
  mutable llvm::BumpPtrAllocator _alloc;
};

// A ExportedSymbolRenameFile is a virtual archive file for dllexported symbols.
//
// One usually has to specify the exact symbol name to resolve it. That's true
// in most cases for PE/COFF, except the one described below.
//
// DLLExported symbols can be specified using a module definition file. In a
// file, one can write an EXPORT directive followed by symbol names. Such
// symbols may not be fully decorated.
//
// If a symbol FOO is specified to be dllexported by a module definition file,
// linker has to search not only for /FOO/ but also for /FOO@[0-9]+/ for stdcall
// and for /\?FOO@@.+/ for C++. This ambiguous matching semantics does not fit
// well with Resolver.
//
// We could probably modify Resolver to resolve ambiguous symbols, but I think
// we don't want to do that because it'd be rarely used, and only this Windows
// specific feature would use it. It's probably not a good idea to make the core
// linker to be able to deal with it.
//
// So, instead of tweaking Resolver, we chose to do some hack here. An
// ExportedSymbolRenameFile maintains a set containing all possibly defined
// symbol names. That set would be a union of (1) all the defined symbols that
// are already parsed and read and (2) all the defined symbols in archive files
// that are not yet be parsed.
//
// If Resolver asks this file to return an atom for a dllexported symbol, find()
// looks up the set, doing ambiguous matching. If there's a symbol with @
// prefix, it returns an atom to rename the dllexported symbol, hoping that
// Resolver will find the new symbol with atsign from an archive file at the
// next visit.
class ExportedSymbolRenameFile : public impl::VirtualArchiveLibraryFile {
public:
  ExportedSymbolRenameFile(const PECOFFLinkingContext &ctx)
      : VirtualArchiveLibraryFile("<export>"),
        _ctx(const_cast<PECOFFLinkingContext *>(&ctx)) {
    for (PECOFFLinkingContext::ExportDesc &desc : _ctx->getDllExports())
      _exportedSyms.insert(desc.name);
  }

  const File *find(StringRef sym, bool dataSymbolOnly) const override {
    typedef PECOFFLinkingContext::ExportDesc ExportDesc;
    if (_exportedSyms.count(sym) == 0)
      return nullptr;
    std::string replace;
    if (!findDecoratedSymbol(_ctx, sym.str(), replace))
      return nullptr;

    for (ExportDesc &exp : _ctx->getDllExports())
      if (exp.name == sym)
        exp.mangledName = replace;
    if (_ctx->deadStrip())
      _ctx->addDeadStripRoot(_ctx->allocate(replace));
    return new (_alloc) impl::SymbolRenameFile(sym, replace);
  }

private:
  std::set<std::string> _exportedSyms;
  mutable llvm::BumpPtrAllocator _alloc;
  mutable PECOFFLinkingContext *_ctx;
};

// Windows has not only one but many entry point functions. The
// appropriate one is automatically selected based on the subsystem
// setting and the user-supplied entry point function.
//
// http://msdn.microsoft.com/en-us/library/f9t8842e.aspx
class EntryPointFile : public SimpleFile {
public:
  EntryPointFile(const PECOFFLinkingContext &ctx)
      : SimpleFile("<entry>"), _ctx(const_cast<PECOFFLinkingContext *>(&ctx)),
        _firstTime(true) {}

  const atom_collection<UndefinedAtom> &undefined() const override {
    return const_cast<EntryPointFile *>(this)->getUndefinedAtoms();
  }

private:
  const atom_collection<UndefinedAtom> &getUndefinedAtoms() {
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_firstTime)
      return _undefinedAtoms;
    _firstTime = false;

    if (_ctx->hasEntry()) {
      StringRef entrySym = _ctx->allocate(getEntry());
      _undefinedAtoms._atoms.push_back(
          new (_alloc) SimpleUndefinedAtom(*this, entrySym));
      _ctx->setHasEntry(true);
      _ctx->setEntrySymbolName(entrySym);
      if (_ctx->deadStrip())
        _ctx->addDeadStripRoot(entrySym);
    }
    return _undefinedAtoms;
  }

  // Returns the entry point function name.
  std::string getEntry() const {
    StringRef opt = _ctx->getEntrySymbolName();
    if (!opt.empty()) {
      std::string mangled;
      if (findDecoratedSymbol(_ctx, opt, mangled))
        return mangled;
      return _ctx->decorateSymbol(opt);
    }
    return _ctx->decorateSymbol(getDefaultEntry());
  }

  std::string getDefaultEntry() const {
    const std::string wWinMainCRTStartup = "wWinMainCRTStartup";
    const std::string WinMainCRTStartup = "WinMainCRTStartup";
    const std::string wmainCRTStartup = "wmainCRTStartup";
    const std::string mainCRTStartup = "mainCRTStartup";

    if (_ctx->isDll()) {
      if (_ctx->getMachineType() == llvm::COFF::IMAGE_FILE_MACHINE_I386)
        return "_DllMainCRTStartup@12";
      return "_DllMainCRTStartup";
    }

    // Returns true if a given name exists in an input object file.
    auto defined = [&](StringRef name) -> bool {
      StringRef sym = _ctx->decorateSymbol(name);
      if (_ctx->definedSymbols().count(sym))
        return true;
      std::string ignore;
      return findDecoratedSymbol(_ctx, sym, ignore);
    };

    switch (_ctx->getSubsystem()) {
    case WindowsSubsystem::IMAGE_SUBSYSTEM_UNKNOWN: {
      if (defined("wWinMain"))
        return wWinMainCRTStartup;
      if (defined("WinMain"))
        return WinMainCRTStartup;
      if (defined("wmain"))
        return wmainCRTStartup;
      if (!defined("main"))
        llvm::errs() << "Cannot infer subsystem; assuming /subsystem:console\n";
      return mainCRTStartup;
    }
    case WindowsSubsystem::IMAGE_SUBSYSTEM_WINDOWS_GUI:
      if (defined("WinMain"))
        return WinMainCRTStartup;
      return wWinMainCRTStartup;
    case WindowsSubsystem::IMAGE_SUBSYSTEM_WINDOWS_CUI:
      if (defined("wmain"))
        return wmainCRTStartup;
      return mainCRTStartup;
    default:
      return mainCRTStartup;
    }
  }

  PECOFFLinkingContext *_ctx;
  atom_collection_vector<UndefinedAtom> _undefinedAtoms;
  std::mutex _mutex;
  llvm::BumpPtrAllocator _alloc;
  bool _firstTime;
};

} // end namespace pecoff
} // end namespace lld
