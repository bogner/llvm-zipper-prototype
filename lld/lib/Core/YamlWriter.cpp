//===- Core/YamlWriter.cpp - Writes YAML ----------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "YamlKeyValues.h"

#include "lld/Core/YamlWriter.h"
#include "lld/Core/Atom.h"
#include "lld/Core/File.h"
#include "lld/Core/Reference.h"

#include "lld/Platform/Platform.h"

#include "llvm/ADT/OwningPtr.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/DataTypes.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/system_error.h"

#include <vector>

namespace lld {
namespace yaml {

namespace {
///
/// In most cases, atoms names are unambiguous, so references can just
/// use the atom name as the target (e.g. target: foo).  But in a few 
/// cases that does not work, so ref-names are added.  These are labels
/// used only in yaml.  The labels do not exist in the Atom model.
///
/// One need for ref-names are when atoms have no user supplied name
/// (e.g. c-string literal).  Another case is when two object files with
/// identically named static functions are merged (ld -r) into one object file.
/// In that case referencing the function by name is ambiguous, so a unique
/// ref-name is added.
///
class RefNameBuilder {
public:
  RefNameBuilder(const File& file) 
                : _collisionCount(0), _unnamedCounter(0) { 
    // visit all atoms
    for(File::defined_iterator it=file.definedAtomsBegin(), 
                              end=file.definedAtomsEnd(); 
                               it != end; ++it) {
      const DefinedAtom* atom = *it;  
      // Build map of atoms names to detect duplicates
      if ( ! atom->name().empty() )
        buildDuplicateNameMap(*atom);
      
      // Find references to unnamed atoms and create ref-names for them.
      for (auto rit=atom->referencesBegin(), rend=atom->referencesEnd();
                                                        rit != rend; ++rit) {
        const Reference* ref = *rit;
        // create refname for any unnamed reference target
        if ( ref->target()->name().empty() ) {
          std::string Storage;
          llvm::raw_string_ostream Buffer(Storage);
          Buffer << llvm::format("L%03d", _unnamedCounter++);
          _refNames[ref->target()] = Buffer.str();
        }
      }
    }
    for(File::undefined_iterator it=file.undefinedAtomsBegin(), 
                              end=file.undefinedAtomsEnd(); 
                               it != end; ++it) {
      buildDuplicateNameMap(**it);
    }
    for(File::shared_library_iterator it=file.sharedLibraryAtomsBegin(), 
                              end=file.sharedLibraryAtomsEnd(); 
                               it != end; ++it) {
      buildDuplicateNameMap(**it);
    }
    for(File::absolute_iterator it=file.absoluteAtomsBegin(), 
                              end=file.absoluteAtomsEnd(); 
                               it != end; ++it) {
      buildDuplicateNameMap(**it);
    }

  
  }
                           
  void buildDuplicateNameMap(const Atom& atom) {
    assert(!atom.name().empty());
    NameToAtom::iterator pos = _nameMap.find(atom.name());
    if ( pos != _nameMap.end() ) {
      // Found name collision, give each a unique ref-name.
      std::string Storage;
      llvm::raw_string_ostream Buffer(Storage);
      Buffer << atom.name() << llvm::format(".%03d", ++_collisionCount);
      _refNames[&atom] = Buffer.str();
      const Atom* prevAtom = pos->second;
      AtomToRefName::iterator pos2 = _refNames.find(prevAtom);
      if ( pos2 == _refNames.end() ) {
        // only create ref-name for previous if none already created
        Buffer << prevAtom->name() << llvm::format(".%03d", ++_collisionCount);
        _refNames[prevAtom] = Buffer.str();
      }
    }
    else {
      // First time we've seen this name, just add it to map.
      _nameMap[atom.name()] = &atom;
    }
  }
  
  bool hasRefName(const Atom* atom) {
     return _refNames.count(atom);
  }
  
  llvm::StringRef refName(const Atom* atom) {
     return _refNames.find(atom)->second;
  }
  
private:
  typedef llvm::StringMap<const Atom*> NameToAtom;
  typedef llvm::DenseMap<const Atom*, std::string> AtomToRefName;
  
  unsigned int      _collisionCount;
  unsigned int      _unnamedCounter;
  NameToAtom        _nameMap;
  AtomToRefName     _refNames;
};


///
/// Helper class for writeObjectText() to write out atoms in yaml format.
///
class AtomWriter {
public:
  AtomWriter(const File& file, Platform& platform, RefNameBuilder& rnb) 
    : _file(file), _platform(platform), _rnb(rnb), _firstAtom(true) { }


  void write(llvm::raw_ostream& out) {
    // write header 
    out << "---\n";
    
    // visit all atoms
    for(File::defined_iterator it=_file.definedAtomsBegin(), 
                              end=_file.definedAtomsEnd(); 
                               it != end; ++it) {
      writeDefinedAtom(**it, out);
    }
    for(File::undefined_iterator it=_file.undefinedAtomsBegin(), 
                              end=_file.undefinedAtomsEnd(); 
                               it != end; ++it) {
      writeUndefinedAtom(**it, out);
    }
    for(File::shared_library_iterator it=_file.sharedLibraryAtomsBegin(), 
                              end=_file.sharedLibraryAtomsEnd(); 
                               it != end; ++it) {
      writeSharedLibraryAtom(**it, out);
    }
    for(File::absolute_iterator it=_file.absoluteAtomsBegin(), 
                              end=_file.absoluteAtomsEnd(); 
                               it != end; ++it) {
      writeAbsoluteAtom(**it, out);
    }
    
    out << "...\n";
  }

  
  void writeDefinedAtom(const DefinedAtom &atom, llvm::raw_ostream& out) {
    if ( _firstAtom ) {
      out << "atoms:\n";
      _firstAtom = false;
    }
    else {
      // add blank line between atoms for readability
      out << "\n";
    }
    
    bool hasDash = false;
    if ( !atom.name().empty() ) {
      out   << "    - "
            << KeyValues::nameKeyword
            << ":"
            << spacePadding(KeyValues::nameKeyword)
            << atom.name() 
            << "\n";
      hasDash = true;
    }
     
    if ( _rnb.hasRefName(&atom) ) {
      out   << (hasDash ? "      " : "    - ")
            << KeyValues::refNameKeyword
            << ":"
            << spacePadding(KeyValues::refNameKeyword)
            << _rnb.refName(&atom) 
            << "\n";
      hasDash = true;
    }
    
    if ( atom.definition() != KeyValues::definitionDefault ) {
      out   << (hasDash ? "      " : "    - ")
            << KeyValues::definitionKeyword 
            << ":"
            << spacePadding(KeyValues::definitionKeyword)
            << KeyValues::definition(atom.definition()) 
            << "\n";
      hasDash = true;
    }
    
    if ( atom.scope() != KeyValues::scopeDefault ) {
      out   << (hasDash ? "      " : "    - ")
            << KeyValues::scopeKeyword 
            << ":"
            << spacePadding(KeyValues::scopeKeyword)
            << KeyValues::scope(atom.scope()) 
            << "\n";
      hasDash = true;
    }
    
     if ( atom.interposable() != KeyValues::interposableDefault ) {
      out   << "      " 
            << KeyValues::interposableKeyword 
            << ":"
            << spacePadding(KeyValues::interposableKeyword)
            << KeyValues::interposable(atom.interposable()) 
            << "\n";
    }
    
    if ( atom.merge() != KeyValues::mergeDefault ) {
      out   << "      " 
            << KeyValues::mergeKeyword 
            << ":"
            << spacePadding(KeyValues::mergeKeyword)
            << KeyValues::merge(atom.merge()) 
            << "\n";
    }
    
    if ( atom.contentType() != KeyValues::contentTypeDefault ) {
      out   << "      " 
            << KeyValues::contentTypeKeyword 
            << ":"
            << spacePadding(KeyValues::contentTypeKeyword)
            << KeyValues::contentType(atom.contentType()) 
            << "\n";
    }

    if ( atom.deadStrip() != KeyValues::deadStripKindDefault ) {
      out   << "      " 
            << KeyValues::deadStripKindKeyword 
            << ":"
            << spacePadding(KeyValues::deadStripKindKeyword)
            << KeyValues::deadStripKind(atom.deadStrip()) 
            << "\n";
    }

    if ( atom.sectionChoice() != KeyValues::sectionChoiceDefault ) {
      out   << "      " 
            << KeyValues::sectionChoiceKeyword 
            << ":"
            << spacePadding(KeyValues::sectionChoiceKeyword)
            << KeyValues::sectionChoice(atom.sectionChoice()) 
            << "\n";
      assert( ! atom.customSectionName().empty() );
      out   << "      " 
            << KeyValues::sectionNameKeyword 
            << ":"
            << spacePadding(KeyValues::sectionNameKeyword)
            << atom.customSectionName()
            << "\n";
    }

    if ( atom.isThumb() != KeyValues::isThumbDefault ) {
      out   << "      " 
            << KeyValues::isThumbKeyword 
            << ":"
            << spacePadding(KeyValues::isThumbKeyword)
            << KeyValues::isThumb(atom.isThumb()) 
            << "\n";
    }

    if ( atom.isAlias() != KeyValues::isAliasDefault ) {
      out   << "      " 
            << KeyValues::isAliasKeyword 
            << ":"
            << spacePadding(KeyValues::isAliasKeyword)
            << KeyValues::isAlias(atom.isAlias()) 
            << "\n";
    }

    if ( (atom.contentType() != DefinedAtom::typeZeroFill) 
                                   && (atom.size() != 0) ) {
      out   << "      " 
            << KeyValues::contentKeyword 
            << ":"
            << spacePadding(KeyValues::contentKeyword)
            << "[ ";
      llvm::ArrayRef<uint8_t> arr = atom.rawContent();
      bool needComma = false;
      for (unsigned int i=0; i < arr.size(); ++i) {
        if ( needComma )
          out  << ", ";
        out  << hexdigit(arr[i] >> 4);
        out  << hexdigit(arr[i] & 0x0F);
        needComma = true;
      }
      out  << " ]\n";
    }

    bool wroteFirstFixup = false;
    for (auto it=atom.referencesBegin(), end=atom.referencesEnd();
                                                    it != end; ++it) {
      const Reference* ref = *it;
      if ( !wroteFirstFixup ) {
        out  << "      fixups:\n";
        wroteFirstFixup = true;
      }
      out   << "      - "
            << KeyValues::fixupsOffsetKeyword
            << ":"
            << spacePadding(KeyValues::fixupsOffsetKeyword)
            << ref->offsetInAtom()
            << "\n";
      out   << "        "
            << KeyValues::fixupsKindKeyword
            << ":"
            << spacePadding(KeyValues::fixupsKindKeyword)
            << _platform.kindToString(ref->kind())
            << "\n";
      const Atom* target = ref->target();
      if ( target != NULL ) {
        llvm::StringRef refName = target->name();
        if ( _rnb.hasRefName(target) )
          refName = _rnb.refName(target);
        assert(!refName.empty());
        out   << "        "
              << KeyValues::fixupsTargetKeyword
              << ":"
              << spacePadding(KeyValues::fixupsTargetKeyword)
              << refName 
              << "\n";
      }
      if ( ref->addend() != 0 ) {
        out   << "        "
              << KeyValues::fixupsAddendKeyword
              << ":"
              << spacePadding(KeyValues::fixupsAddendKeyword)
              << ref->addend()
              << "\n";
      }
    }
  }
    

  void writeUndefinedAtom(const UndefinedAtom &atom, llvm::raw_ostream& out) {
    if ( _firstAtom ) {
      out  << "atoms:\n";
      _firstAtom = false;
    }
    else {
      // add blank line between atoms for readability
      out  << "\n";
    }
        
    out   << "    - "
          << KeyValues::nameKeyword
          << ":"
          << spacePadding(KeyValues::nameKeyword)
          << atom.name() 
          << "\n";

    out   << "      " 
          << KeyValues::definitionKeyword 
          << ":"
          << spacePadding(KeyValues::definitionKeyword)
          << KeyValues::definition(atom.definition()) 
          << "\n";

    if ( atom.canBeNull() != KeyValues::canBeNullDefault ) {
      out   << "      " 
            << KeyValues::canBeNullKeyword 
            << ":"
            << spacePadding(KeyValues::canBeNullKeyword)
            << KeyValues::canBeNull(atom.canBeNull()) 
            << "\n";
    }
  }

  void writeSharedLibraryAtom(const SharedLibraryAtom& atom, llvm::raw_ostream& out) {
    if ( _firstAtom ) {
      out  << "atoms:\n";
      _firstAtom = false;
    }
    else {
      // add blank line between atoms for readability
      out  << "\n";
    }
        
    out   << "    - "
          << KeyValues::nameKeyword
          << ":"
          << spacePadding(KeyValues::nameKeyword)
          << atom.name() 
          << "\n";

    out   << "      " 
          << KeyValues::definitionKeyword 
          << ":"
          << spacePadding(KeyValues::definitionKeyword)
          << KeyValues::definition(atom.definition()) 
          << "\n";

    if ( !atom.loadName().empty() ) {
      out   << "      " 
            << KeyValues::loadNameKeyword 
            << ":"
            << spacePadding(KeyValues::loadNameKeyword)
            << atom.loadName()
            << "\n";
    }

    if ( atom.canBeNullAtRuntime() ) {
      out   << "      " 
            << KeyValues::canBeNullKeyword 
            << ":"
            << spacePadding(KeyValues::canBeNullKeyword)
            << KeyValues::canBeNull(UndefinedAtom::canBeNullAtRuntime) 
            << "\n";
    }
   }
   
  void writeAbsoluteAtom(const AbsoluteAtom& atom, llvm::raw_ostream& out) {
     if ( _firstAtom ) {
      out << "atoms:\n";
      _firstAtom = false;
    }
    else {
      // add blank line between atoms for readability
      out << "\n";
    }
        
    out   << "    - "
          << KeyValues::nameKeyword
          << ":"
          << spacePadding(KeyValues::nameKeyword)
          << atom.name() 
          << "\n";

    out   << "      " 
          << KeyValues::definitionKeyword 
          << ":"
          << spacePadding(KeyValues::definitionKeyword)
          << KeyValues::definition(atom.definition()) 
          << "\n";
    
    out   << "      " 
          << KeyValues::valueKeyword 
          << ":"
          << spacePadding(KeyValues::valueKeyword)
          << "0x";
     out.write_hex(atom.value());
     out << "\n";
   }
                     

private:
  // return a string of the correct number of spaces to align value
  const char* spacePadding(const char* key) {
    const char* spaces = "                  ";
    assert(strlen(spaces) > strlen(key));
    return &spaces[strlen(key)];
  }

  char hexdigit(uint8_t nibble) {
    if ( nibble < 0x0A )
      return '0' + nibble;
    else
      return 'A' + nibble - 0x0A;
  }

  const File&         _file;
  Platform&           _platform;
  RefNameBuilder&     _rnb;
  bool                _firstAtom;
};

} // anonymous namespace



///
/// writeObjectText - writes the lld::File object as in YAML
/// format to the specified stream.
///
void writeObjectText(const File& file, Platform& platform, 
                      llvm::raw_ostream &out) {
  // Figure what ref-name labels are needed
  RefNameBuilder rnb(file);
  
  // Write out all atoms
  AtomWriter writer(file, platform, rnb);
  writer.write(out);
}

} // namespace yaml
} // namespace lld
