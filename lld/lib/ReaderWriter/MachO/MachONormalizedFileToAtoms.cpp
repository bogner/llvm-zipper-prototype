//===- lib/ReaderWriter/MachO/MachONormalizedFileToAtoms.cpp --------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

///
/// \file Converts from in-memory normalized mach-o to in-memory Atoms.
///
///                  +------------+
///                  | normalized |
///                  +------------+
///                        |
///                        |
///                        v
///                    +-------+
///                    | Atoms |
///                    +-------+

#include "MachONormalizedFile.h"
#include "File.h"
#include "Atoms.h"

#include "lld/Core/Error.h"
#include "lld/Core/LLVM.h"

#include "llvm/Support/MachO.h"

using namespace llvm::MachO;

namespace lld {
namespace mach_o {
namespace normalized {

static uint64_t nextSymbolAddress(const NormalizedFile &normalizedFile,
                                const Symbol &symbol) {
  uint64_t symbolAddr = symbol.value;
  uint8_t symbolSectionIndex = symbol.sect;
  const Section &section = normalizedFile.sections[symbolSectionIndex - 1];
  // If no symbol after this address, use end of section address.
  uint64_t closestAddr = section.address + section.content.size();
  for (const Symbol &s : normalizedFile.globalSymbols) {
    if (s.sect != symbolSectionIndex)
      continue;
    uint64_t sValue = s.value;
    if (sValue <= symbolAddr)
      continue;
    if (sValue < closestAddr)
      closestAddr = s.value;
  }
  for (const Symbol &s : normalizedFile.localSymbols) {
    if (s.sect != symbolSectionIndex)
      continue;
    uint64_t sValue = s.value;
    if (sValue <= symbolAddr)
      continue;
    if (sValue < closestAddr)
      closestAddr = s.value;
  }
  return closestAddr;
}

static Atom::Scope atomScope(uint8_t scope) {
  switch (scope) {
  case N_EXT:
    return Atom::scopeGlobal;
  case N_PEXT | N_EXT:
    return Atom::scopeLinkageUnit;
  case 0:
    return Atom::scopeTranslationUnit;
  }
  llvm_unreachable("unknown scope value!");
}

static DefinedAtom::ContentType atomTypeFromSection(const Section &section) {
  // FIX ME
  return DefinedAtom::typeCode;
}

static void processSymbol(const NormalizedFile &normalizedFile, MachOFile &file,
                          const Symbol &sym, bool copyRefs) {
  // Mach-O symbol table does have size in it, so need to scan ahead
  // to find symbol with next highest address.
  const Section &section = normalizedFile.sections[sym.sect - 1];
  uint64_t offset = sym.value - section.address;
  uint64_t size = nextSymbolAddress(normalizedFile, sym) - sym.value;
  if (section.type == llvm::MachO::S_ZEROFILL){
    file.addZeroFillDefinedAtom(sym.name, atomScope(sym.scope), size, copyRefs);
  } else {
    ArrayRef<uint8_t> atomContent = section.content.slice(offset, size);
    file.addDefinedAtom(sym.name, atomScope(sym.scope),
                        atomTypeFromSection(section), DefinedAtom::mergeNo,
                        atomContent, copyRefs);
  }
}


static void processUndefindeSymbol(MachOFile &file, const Symbol &sym,
                                  bool copyRefs) {
  // Undefinded symbols with n_value!=0 are actually tentative definitions.
  if (sym.value == Hex64(0)) {
    file.addUndefinedAtom(sym.name, copyRefs);
  } else {
    file.addTentativeDefAtom(sym.name, atomScope(sym.scope), sym.value,
                              DefinedAtom::Alignment(sym.desc >> 8), copyRefs);
  }
}

static error_code processSection(MachOFile &file, const Section &section,
                                 bool is64, bool copyRefs) {
  unsigned offset = 0;
  const unsigned pointerSize = (is64 ? 8 : 4);
  switch (section.type) {
  case llvm::MachO::S_REGULAR:
    if (section.segmentName.equals("__TEXT") && 
        section.sectionName.equals("__ustring")) {
      if ((section.content.size() % 4) != 0)
        return make_dynamic_error_code(Twine("Section ") + section.segmentName
                                     + "/" + section.sectionName 
                                     + " has a size that is not even"); 
      for (size_t i = 0, e = section.content.size(); i != e; i +=2) {
        if ((section.content[i] == 0) && (section.content[i+1] == 0)) {
          unsigned size = i - offset + 2;
          ArrayRef<uint8_t> utf16Content = section.content.slice(offset, size);
          file.addDefinedAtom(StringRef(), DefinedAtom::scopeLinkageUnit,
                              DefinedAtom::typeUTF16String,
                              DefinedAtom::mergeByContent, utf16Content,
                              copyRefs);
          offset = i + 2;
        }
      }
      if (offset != section.content.size()) {
        return make_dynamic_error_code(Twine("Section ") + section.segmentName
                                       + "/" + section.sectionName 
                                       + " is supposed to contain 0x0000 "
                                       "terminated UTF16 strings, but the "
                                       "last string in the section is not zero "
                                       "terminated."); 
      }
    }
    break;
  case llvm::MachO::S_COALESCED:
  case llvm::MachO::S_ZEROFILL:
    // These sections are broken into atoms based on symbols.
    break;
  case S_MOD_INIT_FUNC_POINTERS:
    if ((section.content.size() % pointerSize) != 0) {
      return make_dynamic_error_code(Twine("Section ") + section.segmentName
                                     + "/" + section.sectionName
                                     + " has type S_MOD_INIT_FUNC_POINTERS but "
                                     "its size ("
                                     + Twine(section.content.size())
                                     + ") is not a multiple of "
                                     + Twine(pointerSize));
    }
    for (size_t i = 0, e = section.content.size(); i != e; i += pointerSize) {
      ArrayRef<uint8_t> bytes = section.content.slice(offset, pointerSize);
      file.addDefinedAtom(StringRef(), DefinedAtom::scopeTranslationUnit,
                          DefinedAtom::typeInitializerPtr, DefinedAtom::mergeNo,
                          bytes, copyRefs);
      offset += pointerSize;
    }
    break;
  case S_MOD_TERM_FUNC_POINTERS:
    if ((section.content.size() % pointerSize) != 0) {
      return make_dynamic_error_code(Twine("Section ") + section.segmentName
                                     + "/" + section.sectionName
                                     + " has type S_MOD_TERM_FUNC_POINTERS but "
                                     "its size ("
                                     + Twine(section.content.size())
                                     + ") is not a multiple of "
                                     + Twine(pointerSize));
    }
    for (size_t i = 0, e = section.content.size(); i != e; i += pointerSize) {
      ArrayRef<uint8_t> bytes = section.content.slice(offset, pointerSize);
      file.addDefinedAtom(StringRef(), DefinedAtom::scopeTranslationUnit,
                          DefinedAtom::typeTerminatorPtr, DefinedAtom::mergeNo,
                          bytes, copyRefs);
      offset += pointerSize;
    }
    break;
  case S_NON_LAZY_SYMBOL_POINTERS:
    if ((section.content.size() % pointerSize) != 0) {
      return make_dynamic_error_code(Twine("Section ") + section.segmentName
                                     + "/" + section.sectionName
                                     + " has type S_NON_LAZY_SYMBOL_POINTERS "
                                     "but its size ("
                                     + Twine(section.content.size())
                                     + ") is not a multiple of "
                                     + Twine(pointerSize));
    }
    for (size_t i = 0, e = section.content.size(); i != e; i += pointerSize) {
      ArrayRef<uint8_t> bytes = section.content.slice(offset, pointerSize);
      file.addDefinedAtom(StringRef(), DefinedAtom::scopeLinkageUnit,
                          DefinedAtom::typeGOT, DefinedAtom::mergeByContent,
                          bytes, copyRefs);
      offset += pointerSize;
    }
    break;
  case llvm::MachO::S_CSTRING_LITERALS:
    for (size_t i = 0, e = section.content.size(); i != e; ++i) {
      if (section.content[i] == 0) {
        unsigned size = i - offset + 1;
        ArrayRef<uint8_t> strContent = section.content.slice(offset, size);
        file.addDefinedAtom(StringRef(), DefinedAtom::scopeLinkageUnit,
                            DefinedAtom::typeCString,
                            DefinedAtom::mergeByContent, strContent, copyRefs);
        offset = i + 1;
      }
    }
    if (offset != section.content.size()) {
      return make_dynamic_error_code(Twine("Section ") + section.segmentName
                                     + "/" + section.sectionName 
                                     + " has type S_CSTRING_LITERALS but the "
                                     "last string in the section is not zero "
                                     "terminated."); 
    }
    break;
  case llvm::MachO::S_4BYTE_LITERALS:
    if ((section.content.size() % 4) != 0)
      return make_dynamic_error_code(Twine("Section ") + section.segmentName
                                     + "/" + section.sectionName 
                                     + " has type S_4BYTE_LITERALS but its "
                                     "size (" + Twine(section.content.size()) 
                                     + ") is not a multiple of 4"); 
    for (size_t i = 0, e = section.content.size(); i != e; i += 4) {
      ArrayRef<uint8_t> byteContent = section.content.slice(offset, 4);
      file.addDefinedAtom(StringRef(), DefinedAtom::scopeLinkageUnit,
                          DefinedAtom::typeLiteral4,
                          DefinedAtom::mergeByContent, byteContent, copyRefs);
      offset += 4;
    }
    break;
  case llvm::MachO::S_8BYTE_LITERALS:
    if ((section.content.size() % 8) != 0)
      return make_dynamic_error_code(Twine("Section ") + section.segmentName
                                     + "/" + section.sectionName 
                                     + " has type S_8YTE_LITERALS but its "
                                     "size (" + Twine(section.content.size()) 
                                     + ") is not a multiple of 8"); 
    for (size_t i = 0, e = section.content.size(); i != e; i += 8) {
      ArrayRef<uint8_t> byteContent = section.content.slice(offset, 8);
      file.addDefinedAtom(StringRef(), DefinedAtom::scopeLinkageUnit,
                          DefinedAtom::typeLiteral8,
                          DefinedAtom::mergeByContent, byteContent, copyRefs);
      offset += 8;
    }
    break;
  case llvm::MachO::S_16BYTE_LITERALS:
    if ((section.content.size() % 16) != 0)
      return make_dynamic_error_code(Twine("Section ") + section.segmentName
                                     + "/" + section.sectionName 
                                     + " has type S_16BYTE_LITERALS but its "
                                     "size (" + Twine(section.content.size()) 
                                     + ") is not a multiple of 16"); 
    for (size_t i = 0, e = section.content.size(); i != e; i += 16) {
      ArrayRef<uint8_t> byteContent = section.content.slice(offset, 16);
      file.addDefinedAtom(StringRef(), DefinedAtom::scopeLinkageUnit,
                          DefinedAtom::typeLiteral16,
                          DefinedAtom::mergeByContent, byteContent, copyRefs);
      offset += 16;
    }
    break;
  default:
    llvm_unreachable("mach-o section type not supported yet");
    break;
  }
  return error_code::success();
}

static ErrorOr<std::unique_ptr<lld::File>>
normalizedObjectToAtoms(const NormalizedFile &normalizedFile, StringRef path,
                        bool copyRefs) {
  std::unique_ptr<MachOFile> file(new MachOFile(path));

  // Create atoms from global symbols.
  for (const Symbol &sym : normalizedFile.globalSymbols) {
    processSymbol(normalizedFile, *file, sym, copyRefs);
  }
  // Create atoms from local symbols.
  for (const Symbol &sym : normalizedFile.localSymbols) {
    processSymbol(normalizedFile, *file, sym, copyRefs);
  }
  // Create atoms from undefinded symbols.
  for (auto &sym : normalizedFile.undefinedSymbols) {
    processUndefindeSymbol(*file, sym, copyRefs);
  }
  // Create atoms from sections that don't have symbols.
  bool is64 = MachOLinkingContext::is64Bit(normalizedFile.arch);
  for (auto &sect : normalizedFile.sections) {
    if (error_code ec = processSection(*file, sect, is64, copyRefs))
      return ec;
  }

  return std::unique_ptr<File>(std::move(file));
}

ErrorOr<std::unique_ptr<lld::File>>
normalizedToAtoms(const NormalizedFile &normalizedFile, StringRef path,
                  bool copyRefs) {
  switch (normalizedFile.fileType) {
  case MH_OBJECT:
    return normalizedObjectToAtoms(normalizedFile, path, copyRefs);
  default:
    llvm_unreachable("unhandled MachO file type!");
  }
}

} // namespace normalized
} // namespace mach_o
} // namespace lld
