//===- Core/SymbolTable.cpp - Main Symbol Table ---------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "lld/Core/SymbolTable.h"
#include "lld/Core/AbsoluteAtom.h"
#include "lld/Core/Atom.h"
#include "lld/Core/DefinedAtom.h"
#include "lld/Core/File.h"
#include "lld/Core/LLVM.h"
#include "lld/Core/Resolver.h"
#include "lld/Core/SharedLibraryAtom.h"
#include "lld/Core/LinkingContext.h"
#include "lld/Core/UndefinedAtom.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMapInfo.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <vector>

namespace lld {
SymbolTable::SymbolTable(const LinkingContext &context) : _context(context) {}

void SymbolTable::add(const UndefinedAtom &atom) { addByName(atom); }

void SymbolTable::add(const SharedLibraryAtom &atom) { addByName(atom); }

void SymbolTable::add(const AbsoluteAtom &atom) { addByName(atom); }

void SymbolTable::add(const DefinedAtom &atom) {
  if (!atom.name().empty() &&
      atom.scope() != DefinedAtom::scopeTranslationUnit) {
    // Named atoms cannot be merged by content.
    assert(atom.merge() != DefinedAtom::mergeByContent);
    // Track named atoms that are not scoped to file (static).
    addByName(atom);
    return;
  }
  if (atom.merge() == DefinedAtom::mergeByContent) {
    // Named atoms cannot be merged by content.
    assert(atom.name().empty());
    addByContent(atom);
  }
}

const Atom *SymbolTable::findGroup(StringRef sym) {
  NameToAtom::iterator pos = _groupTable.find(sym);
  if (pos == _groupTable.end())
    return nullptr;
  return pos->second;
}

bool SymbolTable::addGroup(const DefinedAtom &da) {
  StringRef name = da.name();
  assert(!name.empty());
  const Atom *existing = findGroup(name);
  if (existing == nullptr) {
    _groupTable[name] = &da;
    return true;
  }
  _replacedAtoms[&da] = existing;
  return false;
}

enum NameCollisionResolution {
  NCR_First,
  NCR_Second,
  NCR_DupDef,
  NCR_DupUndef,
  NCR_DupShLib,
  NCR_Error
};

static NameCollisionResolution cases[4][4] = {
  //regular     absolute    undef      sharedLib
  {
    // first is regular
    NCR_DupDef, NCR_Error,   NCR_First, NCR_First
  },
  {
    // first is absolute
    NCR_Error,  NCR_Error,  NCR_First, NCR_First
  },
  {
    // first is undef
    NCR_Second, NCR_Second, NCR_DupUndef, NCR_Second
  },
  {
    // first is sharedLib
    NCR_Second, NCR_Second, NCR_First, NCR_DupShLib
  }
};

static NameCollisionResolution collide(Atom::Definition first,
                                       Atom::Definition second) {
  return cases[first][second];
}

enum MergeResolution {
  MCR_First,
  MCR_Second,
  MCR_Largest,
  MCR_SameSize,
  MCR_Error
};

static MergeResolution mergeCases[][6] = {
  // no          tentative      weak          weakAddress   sameNameAndSize largest
  {MCR_Error,    MCR_First,     MCR_First,    MCR_First,    MCR_SameSize,   MCR_Largest},  // no
  {MCR_Second,   MCR_Largest,   MCR_Second,   MCR_Second,   MCR_SameSize,   MCR_Largest},  // tentative
  {MCR_Second,   MCR_First,     MCR_First,    MCR_Second,   MCR_SameSize,   MCR_Largest},  // weak
  {MCR_Second,   MCR_First,     MCR_First,    MCR_First,    MCR_SameSize,   MCR_Largest},  // weakAddress
  {MCR_SameSize, MCR_SameSize,  MCR_SameSize, MCR_SameSize, MCR_SameSize,   MCR_SameSize}, // sameSize
  {MCR_Largest,  MCR_Largest,   MCR_Largest,  MCR_Largest,  MCR_SameSize,   MCR_Largest},  // largest
};

static MergeResolution mergeSelect(DefinedAtom::Merge first,
                                   DefinedAtom::Merge second) {
  assert(first != DefinedAtom::mergeByContent);
  assert(second != DefinedAtom::mergeByContent);
  return mergeCases[first][second];
}

static uint64_t getSizeFollowReferences(const DefinedAtom *atom, uint32_t kind) {
  uint64_t size = 0;
redo:
  while (atom) {
    for (const Reference *r : *atom) {
      if (r->kindNamespace() == Reference::KindNamespace::all &&
          r->kindArch() == Reference::KindArch::all &&
          r->kindValue() == kind) {
        atom = cast<DefinedAtom>(r->target());
        size += atom->size();
        goto redo;
      }
    }
    break;
  }
  return size;
}

// Returns the size of the section containing the given atom. Atoms in the same
// section are connected by layout-before and layout-after edges, so this
// function traverses them to get the total size of atoms in the same section.
static uint64_t sectionSize(const DefinedAtom *atom) {
  return atom->size()
      + getSizeFollowReferences(atom, lld::Reference::kindLayoutBefore)
      + getSizeFollowReferences(atom, lld::Reference::kindLayoutAfter);
}

void SymbolTable::addByName(const Atom &newAtom) {
  StringRef name = newAtom.name();
  assert(!name.empty());
  const Atom *existing = findByName(name);
  if (existing == nullptr) {
    // Name is not in symbol table yet, add it associate with this atom.
    _nameTable[name] = &newAtom;
    return;
  }

  // Name is already in symbol table and associated with another atom.
  bool useNew = true;
  switch (collide(existing->definition(), newAtom.definition())) {
  case NCR_First:
    useNew = false;
    break;
  case NCR_Second:
    useNew = true;
    break;
  case NCR_DupDef:
    assert(existing->definition() == Atom::definitionRegular);
    assert(newAtom.definition() == Atom::definitionRegular);
    switch (mergeSelect(((DefinedAtom*)existing)->merge(),
                        ((DefinedAtom*)&newAtom)->merge())) {
    case MCR_First:
      useNew = false;
      break;
    case MCR_Second:
      useNew = true;
      break;
    case MCR_Largest: {
      uint64_t existingSize = sectionSize((DefinedAtom*)existing);
      uint64_t newSize = sectionSize((DefinedAtom*)&newAtom);
      useNew = (newSize >= existingSize);
      break;
    }
    case MCR_SameSize: {
      uint64_t existingSize = sectionSize((DefinedAtom*)existing);
      uint64_t newSize = sectionSize((DefinedAtom*)&newAtom);
      if (existingSize == newSize) {
        useNew = true;
        break;
      }
      llvm::errs() << "Size mismatch: "
                   << existing->name() << " (" << existingSize << ") "
                   << newAtom.name() << " (" << newSize << ")\n";
      // fallthrough
    }
    case MCR_Error:
      if (!_context.getAllowDuplicates()) {
        llvm::errs() << "Duplicate symbols: "
                     << existing->name()
                     << ":"
                     << existing->file().path()
                     << " and "
                     << newAtom.name()
                     << ":"
                     << newAtom.file().path()
                     << "\n";
        llvm::report_fatal_error("duplicate symbol error");
      }
      useNew = false;
      break;
    }
    break;
  case NCR_DupUndef: {
    const UndefinedAtom* existingUndef = dyn_cast<UndefinedAtom>(existing);
    const UndefinedAtom* newUndef = dyn_cast<UndefinedAtom>(&newAtom);
    assert(existingUndef != nullptr);
    assert(newUndef != nullptr);

    bool sameCanBeNull = (existingUndef->canBeNull() == newUndef->canBeNull());
    if (!sameCanBeNull &&
        _context.warnIfCoalesableAtomsHaveDifferentCanBeNull()) {
      llvm::errs() << "lld warning: undefined symbol "
                   << existingUndef->name()
                   << " has different weakness in "
                   << existingUndef->file().path()
                   << " and in " << newUndef->file().path() << "\n";
    }

    const UndefinedAtom *existingFallback = existingUndef->fallback();
    const UndefinedAtom *newFallback = newUndef->fallback();
    bool hasDifferentFallback =
        (existingFallback && newFallback &&
         existingFallback->name() != newFallback->name());
    if (hasDifferentFallback) {
      llvm::errs() << "lld warning: undefined symbol "
                   << existingUndef->name() << " has different fallback: "
                   << existingFallback->name() << " in "
                   << existingUndef->file().path() << " and "
                   << newFallback->name() << " in "
                   << newUndef->file().path() << "\n";
    }

    bool hasNewFallback = newUndef->fallback();
    if (sameCanBeNull)
      useNew = hasNewFallback;
    else
      useNew = (newUndef->canBeNull() < existingUndef->canBeNull());
    break;
  }
  case NCR_DupShLib: {
    const SharedLibraryAtom *curShLib = dyn_cast<SharedLibraryAtom>(existing);
    const SharedLibraryAtom *newShLib = dyn_cast<SharedLibraryAtom>(&newAtom);
    assert(curShLib != nullptr);
    assert(newShLib != nullptr);
    bool sameNullness =
        (curShLib->canBeNullAtRuntime() == newShLib->canBeNullAtRuntime());
    bool sameName = curShLib->loadName().equals(newShLib->loadName());
    if (!sameName) {
      useNew = false;
      if (_context.warnIfCoalesableAtomsHaveDifferentLoadName()) {
        // FIXME: need diagonstics interface for writing warning messages
        llvm::errs() << "lld warning: shared library symbol "
                     << curShLib->name() << " has different load path in "
                     << curShLib->file().path() << " and in "
                     << newShLib->file().path();
      }
    } else if (!sameNullness) {
      useNew = false;
      if (_context.warnIfCoalesableAtomsHaveDifferentCanBeNull()) {
        // FIXME: need diagonstics interface for writing warning messages
        llvm::errs() << "lld warning: shared library symbol "
                     << curShLib->name() << " has different weakness in "
                     << curShLib->file().path() << " and in "
                     << newShLib->file().path();
      }
    } else {
      // Both shlib atoms are identical and can be coalesced.
      useNew = false;
    }
    break;
  }
  case NCR_Error:
    llvm::errs() << "SymbolTable: error while merging " << name << "\n";
    llvm::report_fatal_error("duplicate symbol error");
    break;
  }

  if (useNew) {
    // Update name table to use new atom.
    _nameTable[name] = &newAtom;
    // Add existing atom to replacement table.
    _replacedAtoms[existing] = &newAtom;
  } else {
    // New atom is not being used.  Add it to replacement table.
    _replacedAtoms[&newAtom] = existing;
  }
}

unsigned SymbolTable::AtomMappingInfo::getHashValue(const DefinedAtom *atom) {
  auto content = atom->rawContent();
  return llvm::hash_combine(atom->size(),
                            atom->contentType(),
                            llvm::hash_combine_range(content.begin(),
                                                     content.end()));
}

bool SymbolTable::AtomMappingInfo::isEqual(const DefinedAtom * const l,
                                           const DefinedAtom * const r) {
  if (l == r)
    return true;
  if (l == getEmptyKey())
    return false;
  if (r == getEmptyKey())
    return false;
  if (l == getTombstoneKey())
    return false;
  if (r == getTombstoneKey())
    return false;

  if (l->contentType() != r->contentType())
    return false;
  if (l->size() != r->size())
    return false;
  ArrayRef<uint8_t> lc = l->rawContent();
  ArrayRef<uint8_t> rc = r->rawContent();
  return memcmp(lc.data(), rc.data(), lc.size()) == 0;
}

void SymbolTable::addByContent(const DefinedAtom & newAtom) {
  // Currently only read-only constants can be merged.
  assert(newAtom.permissions() == DefinedAtom::permR__);
  AtomContentSet::iterator pos = _contentTable.find(&newAtom);
  if (pos == _contentTable.end()) {
    _contentTable.insert(&newAtom);
    return;
  }
  const Atom* existing = *pos;
  // New atom is not being used.  Add it to replacement table.
  _replacedAtoms[&newAtom] = existing;
}

const Atom *SymbolTable::findByName(StringRef sym) {
  NameToAtom::iterator pos = _nameTable.find(sym);
  if (pos == _nameTable.end())
    return nullptr;
  return pos->second;
}

bool SymbolTable::isDefined(StringRef sym) {
  if (const Atom *atom = findByName(sym))
    return atom->definition() != Atom::definitionUndefined;
  return false;
}

void SymbolTable::addReplacement(const Atom *replaced,
                                 const Atom *replacement) {
  _replacedAtoms[replaced] = replacement;
}

const Atom *SymbolTable::replacement(const Atom *atom) {
  // Find the replacement for a given atom. Atoms in _replacedAtoms
  // may be chained, so find the last one.
  for (;;) {
    AtomToAtom::iterator pos = _replacedAtoms.find(atom);
    if (pos == _replacedAtoms.end())
      return atom;
    atom = pos->second;
  }
}

unsigned int SymbolTable::size() {
  return _nameTable.size();
}

std::vector<const UndefinedAtom *> SymbolTable::undefines() {
  std::vector<const UndefinedAtom *> ret;
  for (auto it : _nameTable) {
    const Atom *atom = it.second;
    assert(atom != nullptr);
    if (const auto undef = dyn_cast<const UndefinedAtom>(atom)) {
      AtomToAtom::iterator pos = _replacedAtoms.find(undef);
      if (pos != _replacedAtoms.end())
        continue;
      ret.push_back(undef);
    }
  }
  return ret;
}

std::vector<StringRef> SymbolTable::tentativeDefinitions() {
  std::vector<StringRef> ret;
  for (auto entry : _nameTable) {
    const Atom *atom = entry.second;
    StringRef name   = entry.first;
    assert(atom != nullptr);
    if (const DefinedAtom *defAtom = dyn_cast<DefinedAtom>(atom))
      if (defAtom->merge() == DefinedAtom::mergeAsTentative)
        ret.push_back(name);
  }
  return ret;
}

} // namespace lld
