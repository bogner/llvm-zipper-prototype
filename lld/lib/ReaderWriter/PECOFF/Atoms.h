//===- lib/ReaderWriter/PECOFF/Atoms.h ------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_READER_WRITER_PE_COFF_ATOMS_H_
#define LLD_READER_WRITER_PE_COFF_ATOMS_H_

#include "lld/Core/File.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/Object/COFF.h"

#include <vector>

namespace lld {
namespace coff {
class COFFDefinedAtom;

using llvm::object::COFFObjectFile;
using llvm::object::coff_section;
using llvm::object::coff_symbol;

/// A COFFReference represents relocation information for an atom. For
/// example, if atom X has a reference to atom Y with offsetInAtom=8, that
/// means that the address starting at 8th byte of the content of atom X needs
/// to be fixed up so that the address points to atom Y's address.
class COFFReference LLVM_FINAL : public Reference {
public:
  COFFReference(Kind kind) : _target(nullptr), _offsetInAtom(0) {
    _kind = kind;
  }

  COFFReference(const Atom *target, uint32_t offsetInAtom, uint16_t relocType)
      : _target(target), _offsetInAtom(offsetInAtom) {
    setKind(static_cast<Reference::Kind>(relocType));
  }

  virtual const Atom *target() const { return _target; }
  virtual void setTarget(const Atom *newAtom) { _target = newAtom; }

  // Addend is a value to be added to the relocation target. For example, if
  // target=AtomX and addend=4, the relocation address will become the address
  // of AtomX + 4. COFF does not support that sort of relocation, thus addend
  // is always zero.
  virtual Addend addend() const { return 0; }
  virtual void setAddend(Addend) {}

  virtual uint64_t offsetInAtom() const { return _offsetInAtom; }

private:
  const Atom *_target;
  uint32_t _offsetInAtom;
};

class COFFAbsoluteAtom : public AbsoluteAtom {
public:
  COFFAbsoluteAtom(const File &f, StringRef n, const coff_symbol *s)
      : _owningFile(f), _name(n), _symbol(s) {}

  virtual const File &file() const { return _owningFile; }

  virtual Scope scope() const {
    if (_symbol->StorageClass == llvm::COFF::IMAGE_SYM_CLASS_STATIC)
      return scopeTranslationUnit;
    return scopeGlobal;
  }

  virtual StringRef name() const { return _name; }

  virtual uint64_t value() const { return _symbol->Value; }

private:
  const File &_owningFile;
  StringRef _name;
  const coff_symbol *_symbol;
};

class COFFUndefinedAtom : public UndefinedAtom {
public:
  COFFUndefinedAtom(const File &f, StringRef n)
      : _owningFile(f), _name(n) {}

  virtual const File &file() const { return _owningFile; }

  virtual StringRef name() const { return _name; }

  virtual CanBeNull canBeNull() const { return CanBeNull::canBeNullNever; }

private:
  const File &_owningFile;
  StringRef _name;
};

/// The base class of all COFF defined atoms. A derived class of
/// COFFBaseDefinedAtom may represent atoms read from a file or atoms created
/// by the linker. An example of the latter case is the jump table for symbols
/// in a DLL.
class COFFBaseDefinedAtom : public DefinedAtom {
public:
  enum class Kind {
    File, Internal
  };

  virtual const File &file() const { return _file; }
  virtual StringRef name() const { return _name; }
  virtual uint64_t size() const { return _dataref.size(); }
  virtual Interposable interposable() const { return interposeNo; }
  virtual Merge merge() const { return mergeNo; }
  virtual Alignment alignment() const { return Alignment(1); }
  virtual SectionChoice sectionChoice() const { return sectionBasedOnContent; }
  virtual StringRef customSectionName() const { return ""; }
  virtual SectionPosition sectionPosition() const { return sectionPositionAny; }
  virtual DeadStripKind deadStrip() const { return deadStripNormal; }
  virtual bool isAlias() const { return false; }
  virtual ArrayRef<uint8_t> rawContent() const { return _dataref; }

  Kind getKind() const { return _kind; }
  void setKind(Kind kind) { _kind = kind; }

  void addReference(std::unique_ptr<COFFReference> reference) {
    _references.push_back(std::move(reference));
  }

  virtual reference_iterator begin() const {
    return reference_iterator(*this, reinterpret_cast<const void *>(0));
  }

  virtual reference_iterator end() const {
    return reference_iterator(
        *this, reinterpret_cast<const void *>(_references.size()));
  }

protected:
  COFFBaseDefinedAtom(const File &file, StringRef name)
      : _file(file), _name(name), _kind(Kind::Internal) {}

  COFFBaseDefinedAtom(const File &file, StringRef name, ArrayRef<uint8_t> data)
      : _file(file), _name(name), _dataref(data), _kind(Kind::Internal) {}

  COFFBaseDefinedAtom(const File &file, StringRef name,
                      std::vector<uint8_t> *data)
      : _file(file), _name(name), _dataref(*data),
        _data(std::unique_ptr<std::vector<uint8_t>>(data)),
        _kind(Kind::Internal) {}

  void setRawContent(std::vector<uint8_t> *data) {
    _dataref = *data;
    _data = std::unique_ptr<std::vector<uint8_t>>(data);
  }

private:
  virtual const Reference *derefIterator(const void *iter) const {
    size_t index = reinterpret_cast<size_t>(iter);
    return _references[index].get();
  }

  virtual void incrementIterator(const void *&iter) const {
    size_t index = reinterpret_cast<size_t>(iter);
    iter = reinterpret_cast<const void *>(index + 1);
  }

  const File &_file;
  StringRef _name;
  ArrayRef<uint8_t> _dataref;
  std::unique_ptr<std::vector<uint8_t>> _data;
  Kind _kind;
  std::vector<std::unique_ptr<COFFReference>> _references;
};

/// A COFFDefinedAtom represents an atom read from a file.
class COFFDefinedAtom : public COFFBaseDefinedAtom {
public:
  COFFDefinedAtom(const File &file, StringRef name, const coff_symbol *symbol,
                  const coff_section *section, ArrayRef<uint8_t> data,
                  StringRef sectionName, uint64_t ordinal)
      : COFFBaseDefinedAtom(file, name, data), _symbol(symbol),
        _section(section), _sectionName(sectionName), _ordinal(ordinal) {
    setKind(Kind::File);
  }

  virtual uint64_t ordinal() const { return _ordinal; }
  uint64_t originalOffset() const { return _symbol->Value; }
  virtual StringRef getSectionName() const { return _sectionName; }

  static bool classof(const COFFBaseDefinedAtom *atom) {
    return atom->getKind() == Kind::File;
  }

  virtual Scope scope() const {
    if (!_symbol)
      return scopeTranslationUnit;
    switch (_symbol->StorageClass) {
    case llvm::COFF::IMAGE_SYM_CLASS_EXTERNAL:
      return scopeGlobal;
    case llvm::COFF::IMAGE_SYM_CLASS_STATIC:
      return scopeTranslationUnit;
    }
    llvm_unreachable("Unknown scope!");
  }

  virtual ContentType contentType() const {
    if (_section->Characteristics & llvm::COFF::IMAGE_SCN_CNT_CODE)
      return typeCode;
    if (_section->Characteristics & llvm::COFF::IMAGE_SCN_CNT_INITIALIZED_DATA)
      return typeData;
    if (_section->Characteristics &
        llvm::COFF::IMAGE_SCN_CNT_UNINITIALIZED_DATA)
      return typeZeroFill;
    return typeUnknown;
  }

  virtual ContentPermissions permissions() const {
    if (_section->Characteristics & llvm::COFF::IMAGE_SCN_MEM_READ &&
        _section->Characteristics & llvm::COFF::IMAGE_SCN_MEM_WRITE)
      return permRW_;
    if (_section->Characteristics & llvm::COFF::IMAGE_SCN_MEM_READ &&
        _section->Characteristics & llvm::COFF::IMAGE_SCN_MEM_EXECUTE)
      return permR_X;
    if (_section->Characteristics & llvm::COFF::IMAGE_SCN_MEM_READ)
      return permR__;
    return perm___;
  }

private:
  const coff_symbol *_symbol;
  const coff_section *_section;
  StringRef _sectionName;
  std::vector<std::unique_ptr<COFFReference>> _references;
  uint64_t _ordinal;
};

class COFFSharedLibraryAtom : public SharedLibraryAtom {
public:
  COFFSharedLibraryAtom(const File &file, StringRef symbolName,
                        StringRef originalName, StringRef loadName)
      : _file(file), _symbolName(symbolName), _loadName(loadName),
        _originalName(originalName) {}

  virtual const File &file() const { return _file; }
  virtual StringRef name() const { return _symbolName; }
  virtual StringRef loadName() const { return _loadName; }
  virtual bool canBeNullAtRuntime() const { return false; }
  virtual StringRef originalName() const { return _originalName; }

private:
  const File &_file;
  StringRef _symbolName;
  StringRef _loadName;
  StringRef _originalName;
};

//===----------------------------------------------------------------------===//
//
// Utility functions to handle layout edges.
//
//===----------------------------------------------------------------------===//

template<typename T, typename U>
void addLayoutEdge(T *a, U *b, lld::Reference::Kind kind) {
  auto ref = new COFFReference(kind);
  ref->setTarget(b);
  a->addReference(std::unique_ptr<COFFReference>(ref));
}

template<typename T, typename U>
void connectWithLayoutEdge(T *a, U *b) {
  addLayoutEdge(a, b, lld::Reference::kindLayoutAfter);
  addLayoutEdge(b, a, lld::Reference::kindLayoutBefore);
}

/// Connect atoms with layout-{before,after} edges. It usually serves two
/// purposes.
///
///   - To prevent atoms from being GC'ed (aka dead-stripped) if there is a
///     reference to one of the atoms. In that case we want to emit all the
///     atoms appeared in the same section, because the referenced "live" atom
///     may reference other atoms in the same section. If we don't add layout
///     edges between atoms, unreferenced atoms in the same section would be
///     GC'ed.
///   - To preserve the order of atmos. We want to emit the atoms in the
///     same order as they appeared in the input object file.
template<typename T>
void connectAtomsWithLayoutEdge(std::vector<T *> &atoms) {
  if (atoms.size() < 2)
    return;
  for (auto it = atoms.begin(), e = atoms.end(); it + 1 != e; ++it)
    connectWithLayoutEdge(*it, *(it + 1));
}

} // namespace coff
} // namespace lld

#endif
