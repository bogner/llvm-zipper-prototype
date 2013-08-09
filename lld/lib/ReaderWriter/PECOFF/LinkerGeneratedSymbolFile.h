//===- lib/ReaderWriter/PECOFF/LinkerGeneratedSymbolFile.cpp --------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Atoms.h"
#include "GroupedSectionsPass.h"
#include "IdataPass.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/Path.h"
#include "lld/Core/ArchiveLibraryFile.h"
#include "lld/Core/InputFiles.h"
#include "lld/Core/PassManager.h"
#include "lld/Passes/LayoutPass.h"
#include "lld/ReaderWriter/PECOFFLinkingContext.h"
#include "lld/ReaderWriter/Reader.h"
#include "lld/ReaderWriter/Simple.h"
#include "lld/ReaderWriter/Writer.h"

namespace lld {
namespace coff {

namespace {

// The symbol ___ImageBase is a linker generated symbol. No standard library
// files define it, but the linker is expected to prepare it as if it was read
// from a file. The content of the atom is a 4-byte integer equal to the image
// base address.
class ImageBaseAtom : public COFFLinkerInternalAtom {
public:
  ImageBaseAtom(const File &file, uint32_t imageBase)
      : COFFLinkerInternalAtom(file, assembleRawContent(imageBase)) {}

  virtual StringRef name() const { return "___ImageBase"; }
  virtual uint64_t ordinal() const { return 0; }
  virtual ContentType contentType() const { return typeData; }
  virtual ContentPermissions permissions() const { return permRW_; }
  virtual DeadStripKind deadStrip() const { return deadStripAlways; }

private:
  std::vector<uint8_t> assembleRawContent(uint32_t imageBase) {
    std::vector<uint8_t> data = std::vector<uint8_t>(4);
    *(reinterpret_cast<uint32_t *>(&data[0])) = imageBase;
    return data;
  }
};

// The file to wrap ImageBaseAtom. This is the only member file of
// LinkerGeneratedSymbolFile.
class MemberFile : public SimpleFile {
public:
  MemberFile(const PECOFFLinkingContext &context)
      : SimpleFile(context, "Member of the Linker Internal File"),
        _atom(*this, context.getBaseAddress()) {
    addAtom(_atom);
  };

private:
  ImageBaseAtom _atom;
};

} // anonymous namespace

// A pseudo library file to wrap MemberFile, which in turn wraps ImageBaseAtom.
// The file the core linker handle is this.
//
// The reason why we don't pass MemberFile to the core linker is because, if we
// did so, ImageBaseAtom would always be emit to the resultant executable. By
// wrapping the file by a library file, we made it to emit ImageBaseAtom only
// when the atom is really referenced.
class LinkerGeneratedSymbolFile : public ArchiveLibraryFile {
public:
  LinkerGeneratedSymbolFile(const PECOFFLinkingContext &context)
      : ArchiveLibraryFile(context, "Linker Internal File"),
        _memberFile(context) {};

  virtual const File *find(StringRef name, bool dataSymbolOnly) const {
    if (name == "___ImageBase")
      return &_memberFile;
    return nullptr;
  }

  virtual const atom_collection<DefinedAtom> &defined() const {
    return _noDefinedAtoms;
  }

  virtual const atom_collection<UndefinedAtom> &undefined() const {
    return _noUndefinedAtoms;
  }

  virtual const atom_collection<SharedLibraryAtom> &sharedLibrary() const {
    return _noSharedLibraryAtoms;
  }

  virtual const atom_collection<AbsoluteAtom> &absolute() const {
    return _noAbsoluteAtoms;
  }

private:
  MemberFile _memberFile;
};

/// An instance of UndefinedSymbolFile has a list of undefined symbols
/// specified by "/include" command line option. This will be added to the
/// input file list to force the core linker to try to resolve the undefined
/// symbols.
class UndefinedSymbolFile : public SimpleFile {
public:
  UndefinedSymbolFile(const LinkingContext &ti)
      : SimpleFile(ti, "Linker Internal File") {
    for (StringRef symbol : ti.initialUndefinedSymbols()) {
      UndefinedAtom *atom = new (_alloc) coff::COFFUndefinedAtom(*this, symbol);
      addAtom(*atom);
    }
  }

private:
  llvm::BumpPtrAllocator _alloc;
};

} // end namespace coff
} // end namespace lld
