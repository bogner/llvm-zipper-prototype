//===- lib/ReaderWriter/PECOFF/IdataPass.h---------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
///
/// \file \brief This linker pass creates atoms for the DLL import
/// information. The defined atoms constructed in this pass will go into .idata
/// section, unless .idata section is merged with other section such as .data.
///
/// For the details of the .idata section format, see Microsoft PE/COFF
/// Specification section 5.4, The .idata Section.
///
//===----------------------------------------------------------------------===//

#ifndef LLD_READER_WRITER_PE_COFF_IDATA_PASS_H
#define LLD_READER_WRITER_PE_COFF_IDATA_PASS_H

#include "Atoms.h"

#include "lld/Core/File.h"
#include "lld/Core/Pass.h"
#include "lld/ReaderWriter/Simple.h"
#include "llvm/Support/COFF.h"

#include <algorithm>
#include <map>

using lld::coff::COFFBaseDefinedAtom;
using lld::coff::COFFDefinedAtom;
using lld::coff::COFFLinkerInternalAtom;
using lld::coff::COFFReference;
using lld::coff::COFFSharedLibraryAtom;
using lld::coff::COFFSharedLibraryAtom;
using llvm::COFF::ImportDirectoryTableEntry;

namespace lld {
namespace pecoff {
namespace idata {

class DLLNameAtom;
class HintNameAtom;
class IdataPassFile;
class ImportTableEntryAtom;

// A state object of this pass.
struct Context {
  Context(MutableFile &f, IdataPassFile &g) : file(f), dummyFile(g) {}

  MutableFile &file;
  IdataPassFile &dummyFile;

  // The object to accumulate idata atoms. Idata atoms need to be grouped by
  // type and be continuous in the output file. To force such layout, we
  // accumulate all atoms created in the pass in the following vectors, and add
  // layout edges when finishing the pass.
  std::vector<COFFBaseDefinedAtom *> importDirectories;
  std::vector<HintNameAtom *> hintNameAtoms;
  std::vector<DLLNameAtom *> dllNameAtoms;

  std::map<StringRef, COFFBaseDefinedAtom *> sharedToDefinedAtom;
};

/// The root class of all idata atoms.
class IdataAtom : public COFFLinkerInternalAtom {
public:
  virtual SectionChoice sectionChoice() const { return sectionCustomRequired; }
  virtual StringRef customSectionName() const { return ".idata"; };
  virtual ContentType contentType() const { return typeData; }
  virtual ContentPermissions permissions() const { return permR__; }

protected:
  IdataAtom(Context &context, std::vector<uint8_t> data);
};

/// A DLLNameAtom contains a name of a DLL and is referenced by the Name RVA
/// field in the import directory table entry.
class DLLNameAtom : public IdataAtom {
public:
  DLLNameAtom(Context &context, StringRef name);
};

/// A HintNameAtom represents a symbol that will be imported from a DLL at
/// runtime. It consists with an optional hint, which is a small integer, and a
/// symbol name.
///
/// A hint is an index of the export pointer table in a DLL. If the import
/// library and DLL is in sync (i.e., ".lib" and ".dll" is for the same version
/// or the symbol ordinal is maintained by hand with ".exp" file), the PE/COFF
/// loader can find the symbol quickly.
class HintNameAtom : public IdataAtom {
public:
  HintNameAtom(Context &context, uint16_t hint, StringRef importName);

  StringRef getContentString() { return _importName; }

private:
  std::vector<uint8_t> assembleRawContent(uint16_t hint, StringRef importName);
  StringRef _importName;
};

class ImportTableEntryAtom : public IdataAtom {
public:
  ImportTableEntryAtom(Context &context, uint32_t contents,
                       StringRef sectionName)
      : IdataAtom(context, assembleRawContent(contents)),
        _sectionName(sectionName) {}

  virtual StringRef customSectionName() const { return _sectionName; };

private:
  std::vector<uint8_t> assembleRawContent(uint32_t contents);
  StringRef _sectionName;
};

/// An ImportDirectoryAtom includes information to load a DLL, including a DLL
/// name, symbols that will be resolved from the DLL, and the import address
/// table that are overwritten by the loader with the pointers to the referenced
/// items. The executable has one ImportDirectoryAtom per one imported DLL.
class ImportDirectoryAtom : public IdataAtom {
public:
  ImportDirectoryAtom(Context &context, StringRef loadName,
                      const std::vector<COFFSharedLibraryAtom *> &sharedAtoms)
      : IdataAtom(context, std::vector<uint8_t>(20, 0)) {
    addRelocations(context, loadName, sharedAtoms);
    context.importDirectories.push_back(this);
  }

  virtual StringRef customSectionName() const { return ".idata.d"; }

private:
  void addRelocations(Context &context, StringRef loadName,
                      const std::vector<COFFSharedLibraryAtom *> &sharedAtoms);

  std::vector<ImportTableEntryAtom *> createImportTableAtoms(
      Context &context, const std::vector<COFFSharedLibraryAtom *> &sharedAtoms,
      bool shouldAddReference, StringRef sectionName) const;
  HintNameAtom *createHintNameAtom(Context &context,
                                   const COFFSharedLibraryAtom *atom) const;

  mutable llvm::BumpPtrAllocator _alloc;
};

/// The last NULL entry in the import directory.
class NullImportDirectoryAtom : public IdataAtom {
public:
  explicit NullImportDirectoryAtom(Context &context)
      : IdataAtom(context, std::vector<uint8_t>(20, 0)) {
    context.importDirectories.push_back(this);
  }

  virtual StringRef customSectionName() const { return ".idata.d"; }
};

// An instance of this class represents "input file" for atoms created in this
// pass. Only one instance of this class is created as a field of IdataPass.
class IdataPassFile : public SimpleFile {
public:
  IdataPassFile(const LinkingContext &ctx)
      : SimpleFile(ctx, "<idata-pass-file>"), _nextOrdinal(0) {
    setOrdinal(ctx.getNextOrdinalAndIncrement());
  }

  uint64_t getNextOrdinal() { return _nextOrdinal++; }

private:
  uint64_t _nextOrdinal;
};

} // namespace idata

class IdataPass : public lld::Pass {
public:
  IdataPass(const LinkingContext &ctx) : _dummyFile(ctx) {}

  virtual void perform(std::unique_ptr<MutableFile> &file);

private:
  std::map<StringRef, std::vector<COFFSharedLibraryAtom *> >
  groupByLoadName(MutableFile &file);

  void createImportDirectory(idata::Context &context, StringRef loadName,
                             std::vector<COFFSharedLibraryAtom *> &dllAtoms);

  template <typename T, typename U>
  void appendAtoms(std::vector<T *> &vec1, const std::vector<U *> &vec2);

  void connectAtoms(idata::Context &context);
  void replaceSharedLibraryAtoms(idata::Context &context);

  // A dummy file with which all the atoms created in the pass will be
  // associated. Atoms need to be associated to an input file even if it's not
  // read from a file, so we use this object.
  idata::IdataPassFile _dummyFile;

  llvm::BumpPtrAllocator _alloc;
};

} // namespace pecoff
} // namespace lld

#endif
