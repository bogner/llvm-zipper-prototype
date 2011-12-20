//===- Core/UndefinedAtom.h - An Undefined Atom ---------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_CORE_UNDEFINED_ATOM_H_
#define LLD_CORE_UNDEFINED_ATOM_H_

#include "lld/Core/Atom.h"

#include "llvm/ADT/StringRef.h"

namespace lld {

/// An UndefinedAtom has no content.
/// It exists as a place holder for a future atom.
class UndefinedAtom : public Atom {
public:
  UndefinedAtom(llvm::StringRef nm)
    : Atom( Atom::definitionUndefined
          , Atom::scopeLinkageUnit
          , Atom::typeUnknown
          , Atom::sectionBasedOnContent
          , false
          , deadStripNormal
          , false
          , false
          , Atom::Alignment(0))
    , _name(nm) {}

  // overrides of Atom
  virtual const File *file() const {
    return 0;
  }

  virtual bool translationUnitSource(llvm::StringRef path) const {
    return false;
  }

  virtual llvm::StringRef name() const {
    return _name;
  }
  virtual uint64_t size() const {
    return 0;
  }
  virtual uint64_t objectAddress() const {
    return 0;
  }
  virtual void copyRawContent(uint8_t buffer[]) const { }
  virtual void setScope(Scope) { }
  bool weakImport();

protected:
  virtual ~UndefinedAtom() {}

  llvm::StringRef _name;
};

} // namespace lld

#endif // LLD_CORE_UNDEFINED_ATOM_H_
