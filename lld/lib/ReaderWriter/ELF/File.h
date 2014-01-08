//===- lib/ReaderWriter/ELF/File.h ----------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_READER_WRITER_ELF_FILE_H
#define LLD_READER_WRITER_ELF_FILE_H

#include "Atoms.h"

#include "lld/Core/File.h"
#include "lld/Core/Reference.h"

#include "lld/ReaderWriter/ELFLinkingContext.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Object/ELF.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/ELF.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/ErrorOr.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/Memory.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/system_error.h"

#include <map>
#include <unordered_map>

namespace lld {
namespace elf {
/// \brief Read a binary, find out based on the symbol table contents what kind
/// of symbol it is and create corresponding atoms for it
template <class ELFT> class ELFFile : public File {
  typedef llvm::object::Elf_Sym_Impl<ELFT> Elf_Sym;
  typedef llvm::object::Elf_Shdr_Impl<ELFT> Elf_Shdr;
  typedef llvm::object::Elf_Rel_Impl<ELFT, false> Elf_Rel;
  typedef llvm::object::Elf_Rel_Impl<ELFT, true> Elf_Rela;
  typedef typename llvm::object::ELFFile<ELFT>::Elf_Sym_Iter Elf_Sym_Iter;

  // A Map is used to hold the atoms that have been divided up
  // after reading the section that contains Merge String attributes
  struct MergeSectionKey {
    MergeSectionKey(const Elf_Shdr *shdr, int32_t offset)
        : _shdr(shdr), _offset(offset) {
    }
    // Data members
    const Elf_Shdr *_shdr;
    int32_t _offset;
  };
  struct MergeSectionEq {
    int64_t operator()(const MergeSectionKey &k) const {
      return llvm::hash_combine((int64_t)(k._shdr->sh_name),
                                (int64_t) k._offset);
    }
    bool operator()(const MergeSectionKey &lhs,
                    const MergeSectionKey &rhs) const {
      return ((lhs._shdr->sh_name == rhs._shdr->sh_name) &&
              (lhs._offset == rhs._offset));
    }
  };

  struct MergeString {
    MergeString(int32_t offset, StringRef str, const Elf_Shdr *shdr,
                StringRef sectionName)
        : _offset(offset), _string(str), _shdr(shdr),
          _sectionName(sectionName) {
    }
    // the offset of this atom
    int32_t _offset;
    // The content
    StringRef _string;
    // Section header
    const Elf_Shdr *_shdr;
    // Section name
    StringRef _sectionName;
  };

  // This is used to find the MergeAtom given a relocation
  // offset
  typedef std::vector<ELFMergeAtom<ELFT> *> MergeAtomsT;

  /// \brief find a mergeAtom given a start offset
  struct FindByOffset {
    const Elf_Shdr *_shdr;
    uint64_t _offset;
    FindByOffset(const Elf_Shdr *shdr, uint64_t offset)
        : _shdr(shdr), _offset(offset) {
    }
    bool operator()(const ELFMergeAtom<ELFT> *a) {
      uint64_t off = a->offset();
      return (_shdr->sh_name == a->section()) &&
             ((_offset >= off) && (_offset <= off + a->size()));
    }
  };

  /// \brief find a merge atom given a offset
  ELFMergeAtom<ELFT> *findMergeAtom(const Elf_Shdr *shdr, uint64_t offset) {
    auto it = std::find_if(_mergeAtoms.begin(), _mergeAtoms.end(),
                           FindByOffset(shdr, offset));
    assert(it != _mergeAtoms.end());
    return *it;
  }

  typedef std::unordered_map<MergeSectionKey, DefinedAtom *, MergeSectionEq,
                             MergeSectionEq> MergedSectionMapT;
  typedef typename MergedSectionMapT::iterator MergedSectionMapIterT;

public:
  ELFFile(StringRef name)
      : File(name, kindObject), _ordinal(0), _doStringsMerge(false),
        _targetHandler(nullptr) {}

  ELFFile(std::unique_ptr<MemoryBuffer> mb, bool atomizeStrings,
          TargetHandlerBase *handler, error_code &ec)
      : File(mb->getBufferIdentifier(), kindObject), _ordinal(0),
        _doStringsMerge(atomizeStrings),
        _targetHandler(reinterpret_cast<TargetHandler<ELFT> *>(handler)) {
    _objFile.reset(new llvm::object::ELFFile<ELFT>(mb.release(), ec));

    if (ec)
      return;

    // Read input sections from the input file that need to be converted to
    // atoms
    if (auto err = createAtomizableSections()) {
      ec = err;
      return;
    }

    // For mergeable strings, we would need to split the section into various
    // atoms
    if (auto err = createMergeableAtoms()) {
      ec = err;
      return;
    }

    // Create the necessary symbols that are part of the section that we
    // created in createAtomizableSections function
    if (auto err = createSymbolsFromAtomizableSections()) {
      ec = err;
      return;
    }

    // Create the appropriate atoms from the file
    if (auto err = createAtoms()) {
      ec = err;
      return;
    }
  }

  Reference::KindArch kindArch() {
    switch (_objFile->getHeader()->e_machine) {
    case llvm::ELF::EM_X86_64:
      return Reference::KindArch::x86_64;
    case llvm::ELF::EM_386:
      return Reference::KindArch::x86;
    case llvm::ELF::EM_ARM:
      return Reference::KindArch::ARM;
    case llvm::ELF::EM_PPC:
      return Reference::KindArch::PowerPC;
    case llvm::ELF::EM_HEXAGON:
      return Reference::KindArch::Hexagon;
    case llvm::ELF::EM_MIPS:
      return Reference::KindArch::Mips;
    }
    llvm_unreachable("unsupported e_machine value");
  }

  /// \brief Read input sections and populate necessary data structures
  /// to read them later and create atoms
  error_code createAtomizableSections() {
    // Handle: SHT_REL and SHT_RELA sections:
    // Increment over the sections, when REL/RELA section types are found add
    // the contents to the RelocationReferences map.
    // Record the number of relocs to guess at preallocating the buffer.
    uint64_t totalRelocs = 0;
    for (auto sit = _objFile->begin_sections(),
              sie = _objFile->end_sections(); sit != sie; ++sit) {
      const Elf_Shdr *section = &*sit;

      if (isIgnoredSection(section))
        continue;

      if (isMergeableStringSection(section)) {
        _mergeStringSections.push_back(section);
        continue;
      }

      // Create a sectionSymbols entry for every progbits section.
      if ((section->sh_type == llvm::ELF::SHT_PROGBITS) ||
          (section->sh_type == llvm::ELF::SHT_INIT_ARRAY) ||
          (section->sh_type == llvm::ELF::SHT_FINI_ARRAY))
        _sectionSymbols[section];

      if (section->sh_type == llvm::ELF::SHT_RELA) {
        auto sHdr = _objFile->getSection(section->sh_info);

        auto sectionName = _objFile->getSectionName(sHdr);
        if (error_code ec = sectionName.getError())
          return ec;

        auto rai(_objFile->begin_rela(section));
        auto rae(_objFile->end_rela(section));

        _relocationAddendReferences[*sectionName] = make_range(rai, rae);
        totalRelocs += std::distance(rai, rae);
      }

      if (section->sh_type == llvm::ELF::SHT_REL) {
        auto sHdr = _objFile->getSection(section->sh_info);

        auto sectionName = _objFile->getSectionName(sHdr);
        if (error_code ec = sectionName.getError())
          return ec;

        auto ri(_objFile->begin_rel(section));
        auto re(_objFile->end_rel(section));

        _relocationReferences[*sectionName] = make_range(ri, re);
        totalRelocs += std::distance(ri, re);
      }
    }
    _references.reserve(totalRelocs);
    return error_code::success();
  }

  /// \brief Create mergeable atoms from sections that have the merge attribute
  /// set
  error_code createMergeableAtoms() {
    // Divide the section that contains mergeable strings into tokens
    // TODO
    // a) add resolver support to recognize multibyte chars
    // b) Create a separate section chunk to write mergeable atoms
    std::vector<MergeString *> tokens;
    for (const Elf_Shdr *msi : _mergeStringSections) {
      auto sectionName = _objFile->getSectionName(msi);
      if (error_code ec = sectionName.getError())
        return ec;

      auto sectionContents = _objFile->getSectionContents(msi);
      if (error_code ec = sectionContents.getError())
        return ec;

      StringRef secCont(
          reinterpret_cast<const char *>(sectionContents->begin()),
          sectionContents->size());

      unsigned int prev = 0;
      for (std::size_t i = 0, e = sectionContents->size(); i != e; ++i) {
        if ((*sectionContents)[i] == '\0') {
          tokens.push_back(new (_readerStorage) MergeString(
              prev, secCont.slice(prev, i + 1), msi, *sectionName));
          prev = i + 1;
        }
      }
    }

    // Create Mergeable atoms
    for (const MergeString *tai : tokens) {
      ArrayRef<uint8_t> content((const uint8_t *)tai->_string.data(),
                                tai->_string.size());
      ELFMergeAtom<ELFT> *mergeAtom = new (_readerStorage) ELFMergeAtom<ELFT>(
          *this, tai->_sectionName, tai->_shdr, content, tai->_offset);
      const MergeSectionKey mergedSectionKey(tai->_shdr, tai->_offset);
      if (_mergedSectionMap.find(mergedSectionKey) == _mergedSectionMap.end())
        _mergedSectionMap.insert(std::make_pair(mergedSectionKey, mergeAtom));
      mergeAtom->setOrdinal(++_ordinal);
      _definedAtoms._atoms.push_back(mergeAtom);
      _mergeAtoms.push_back(mergeAtom);
    }
    return error_code::success();
  }

  /// \brief Add the symbols that the sections contain. The symbols will be
  /// converted to atoms for
  /// Undefined symbols, absolute symbols
  error_code createSymbolsFromAtomizableSections() {
    // Increment over all the symbols collecting atoms and symbol names for
    // later use.
    auto SymI = _objFile->begin_symbols(),
         SymE = _objFile->end_symbols();

    // Skip over dummy sym.
    if (SymI != SymE)
      ++SymI;

    for (; SymI != SymE; ++SymI) {
      const Elf_Shdr *section = _objFile->getSection(&*SymI);

      auto symbolName = _objFile->getSymbolName(SymI);
      if (error_code ec = symbolName.getError())
        return ec;

      if (SymI->st_shndx == llvm::ELF::SHN_ABS) {
        // Create an absolute atom.
        auto *newAtom = new (_readerStorage)
            ELFAbsoluteAtom<ELFT>(*this, *symbolName, &*SymI, SymI->st_value);

        _absoluteAtoms._atoms.push_back(newAtom);
        _symbolToAtomMapping.insert(std::make_pair(&*SymI, newAtom));
      } else if (SymI->st_shndx == llvm::ELF::SHN_UNDEF) {
        // Create an undefined atom.
        auto *newAtom = new (_readerStorage)
            ELFUndefinedAtom<ELFT>(*this, *symbolName, &*SymI);

        _undefinedAtoms._atoms.push_back(newAtom);
        _symbolToAtomMapping.insert(std::make_pair(&*SymI, newAtom));
      } else if (isCommonSymbol(&*SymI)) {
        auto *newAtom = new (_readerStorage)
          ELFCommonAtom<ELFT>(*this, *symbolName, &*SymI);
        newAtom->setOrdinal(++_ordinal);
        _definedAtoms._atoms.push_back(newAtom);
        _symbolToAtomMapping.insert(std::make_pair(&*SymI, newAtom));
      } else {
        assert(section && "Symbol not defined in a section!");
        // This is actually a defined symbol. Add it to its section's list of
        // symbols.
        if (SymI->getType() == llvm::ELF::STT_NOTYPE ||
            SymI->getType() == llvm::ELF::STT_OBJECT ||
            SymI->getType() == llvm::ELF::STT_FUNC ||
            SymI->getType() == llvm::ELF::STT_GNU_IFUNC ||
            SymI->getType() == llvm::ELF::STT_SECTION ||
            SymI->getType() == llvm::ELF::STT_FILE ||
            SymI->getType() == llvm::ELF::STT_TLS) {
          _sectionSymbols[section].push_back(SymI);
        } else {
          llvm::errs() << "Unable to create atom for: " << *symbolName << "\n";
          return llvm::object::object_error::parse_failed;
        }
      }
    }
    return error_code::success();
  }

  /// \brief Create individual atoms
  error_code createAtoms() {
    for (auto &i : _sectionSymbols) {
      const Elf_Shdr *section = i.first;
      std::vector<Elf_Sym_Iter> &symbols = i.second;

      // Sort symbols by position.
      std::stable_sort(symbols.begin(), symbols.end(),
                       [](Elf_Sym_Iter A, Elf_Sym_Iter B) {
        return A->st_value < B->st_value;
      });

      auto sectionName = section ? _objFile->getSectionName(section)
                                 : StringRef();
      if (error_code ec = sectionName.getError())
        return ec;

      auto sectionContents =
          (section && section->sh_type != llvm::ELF::SHT_NOBITS)
              ? _objFile->getSectionContents(section)
              : ArrayRef<uint8_t>();

      if (error_code ec = sectionContents.getError())
        return ec;

      StringRef secCont(
          reinterpret_cast<const char *>(sectionContents->begin()),
          sectionContents->size());

      // If the section has no symbols, create a custom atom for it.
      if (section && section->sh_type == llvm::ELF::SHT_PROGBITS &&
          symbols.empty()) {
        ELFDefinedAtom<ELFT> *newAtom = createSectionAtom(
            section, *sectionName, secCont);
        _definedAtoms._atoms.push_back(newAtom);
        newAtom->setOrdinal(++_ordinal);
        continue;
      }

      ELFDefinedAtom<ELFT> *previousAtom = nullptr;
      ELFReference<ELFT> *anonFollowedBy = nullptr;

      for (auto si = symbols.begin(), se = symbols.end(); si != se; ++si) {
        auto symbol = *si;
        StringRef symbolName = "";
        if (symbol->getType() != llvm::ELF::STT_SECTION) {
          auto symName = _objFile->getSymbolName(symbol);
          if (error_code ec = symName.getError())
            return ec;
          symbolName = *symName;
        }

        uint64_t contentSize = symbolContentSize(
            section, &*symbol, (si + 1 == se) ? nullptr : &**(si + 1));

        // Check to see if we need to add the FollowOn Reference
        ELFReference<ELFT> *followOn = nullptr;
        if (previousAtom) {
          // Replace the followon atom with the anonymous atom that we created,
          // so that the next symbol that we create is a followon from the
          // anonymous atom.
          if (anonFollowedBy) {
            followOn = anonFollowedBy;
          } else {
            followOn = new (_readerStorage)
                ELFReference<ELFT>(lld::Reference::kindLayoutAfter);
            previousAtom->addReference(followOn);
          }
        }

        ArrayRef<uint8_t> symbolData(
            (uint8_t *)sectionContents->data() + symbol->st_value, contentSize);

        // If the linker finds that a section has global atoms that are in a
        // mergeable section, treat them as defined atoms as they shouldn't be
        // merged away as well as these symbols have to be part of symbol
        // resolution
        if (isMergeableStringSection(section)) {
          if (symbol->getBinding() == llvm::ELF::STB_GLOBAL) {
            auto definedMergeAtom = new (_readerStorage) ELFDefinedAtom<ELFT>(
                *this, symbolName, *sectionName, &**si, section, symbolData,
                _references.size(), _references.size(), _references);
            _definedAtoms._atoms.push_back(definedMergeAtom);
            definedMergeAtom->setOrdinal(++_ordinal);
          }
          continue;
        }

        // Don't allocate content to a weak symbol, as they may be merged away.
        // Create an anonymous atom to hold the data.
        ELFDefinedAtom<ELFT> *anonAtom = nullptr;
        anonFollowedBy = nullptr;
        if (symbol->getBinding() == llvm::ELF::STB_WEAK && contentSize != 0) {
          // Create anonymous new non-weak ELF symbol that holds the symbol
          // data.
          auto sym = new (_readerStorage) Elf_Sym(*symbol);
          sym->setBinding(llvm::ELF::STB_GLOBAL);
          anonAtom = createDefinedAtomAndAssignRelocations(
              "", *sectionName, sym, section, symbolData);
          anonAtom->setOrdinal(++_ordinal);
          symbolData = ArrayRef<uint8_t>();

          if (previousAtom)
            createEdge(anonAtom, previousAtom,
                       lld::Reference::kindLayoutBefore);
          // If this is the last atom, let's not create a followon reference.
          if (anonAtom && (si + 1) != se) {
            anonFollowedBy = new (_readerStorage) ELFReference<ELFT>(
                lld::Reference::kindLayoutAfter);
            anonAtom->addReference(anonFollowedBy);
          }
        }

        ELFDefinedAtom<ELFT> *newAtom = createDefinedAtomAndAssignRelocations(
            symbolName, *sectionName, &*symbol, section, symbolData);
        newAtom->setOrdinal(++_ordinal);

        // If the atom was a weak symbol, let's create a followon reference to
        // the anonymous atom that we created.
        if (anonAtom)
          createEdge(newAtom, anonAtom, Reference::kindLayoutAfter);

        if (previousAtom) {
          // Set the followon atom to the weak atom that we have created, so
          // that they would alias when the file gets written.
          followOn->setTarget(anonAtom ? anonAtom : newAtom);

          // Add a preceded-by reference only if the current atom is not a weak
          // atom.
          if (symbol->getBinding() != llvm::ELF::STB_WEAK)
            createEdge(newAtom, previousAtom,
                       lld::Reference::kindLayoutBefore);
        }

        // The previous atom is always the atom created before unless the atom
        // is a weak atom.
        previousAtom = anonAtom ? anonAtom : newAtom;

        _definedAtoms._atoms.push_back(newAtom);
        _symbolToAtomMapping.insert(std::make_pair(&*symbol, newAtom));
        if (anonAtom)
          _definedAtoms._atoms.push_back(anonAtom);
      }
    }

    updateReferences();
    return error_code::success();
  }

  virtual const atom_collection<DefinedAtom> &defined() const {
    return _definedAtoms;
  }

  virtual const atom_collection<UndefinedAtom> &undefined() const {
    return _undefinedAtoms;
  }

  virtual const atom_collection<SharedLibraryAtom> &sharedLibrary() const {
    return _sharedLibraryAtoms;
  }

  virtual const atom_collection<AbsoluteAtom> &absolute() const {
    return _absoluteAtoms;
  }

  TargetHandler<ELFT> *targetHandler() const { return _targetHandler; }

  Atom *findAtom(const Elf_Sym *symbol) {
    return _symbolToAtomMapping.lookup(symbol);
  }

private:

  ELFDefinedAtom<ELFT> *createDefinedAtomAndAssignRelocations(
      StringRef symbolName, StringRef sectionName, const Elf_Sym *symbol,
      const Elf_Shdr *section, ArrayRef<uint8_t> content) {
    unsigned int referenceStart = _references.size();

    // Only relocations that are inside the domain of the atom are added.

    // Add Rela (those with r_addend) references:
    auto rari = _relocationAddendReferences.find(sectionName);
    if (rari != _relocationAddendReferences.end()) {
      for (const Elf_Rela &rai : rari->second) {
        if (rai.r_offset < symbol->st_value ||
            symbol->st_value + content.size() <= rai.r_offset)
          continue;
        bool isMips64EL = _objFile->isMips64EL();
        uint32_t symbolIndex = rai.getSymbol(isMips64EL);
        auto *ERef = new (_readerStorage) ELFReference<ELFT>(
            &rai, rai.r_offset - symbol->st_value, kindArch(),
            rai.getType(isMips64EL), symbolIndex);
        _references.push_back(ERef);
      }
    }

    // Add Rel references.
    auto rri = _relocationReferences.find(sectionName);
    if (rri != _relocationReferences.end()) {
      for (const Elf_Rel &ri : rri->second) {
        if (ri.r_offset < symbol->st_value ||
            symbol->st_value + content.size() <= ri.r_offset)
          continue;
        bool isMips64EL = _objFile->isMips64EL();
        uint32_t symbolIndex = ri.getSymbol(isMips64EL);
        auto *ERef = new (_readerStorage) ELFReference<ELFT>(
            &ri, ri.r_offset - symbol->st_value, kindArch(),
            ri.getType(isMips64EL), symbolIndex);
        // Read the addend from the section contents
        // TODO : We should move the way lld reads relocations totally from
        // ELFFile
        int32_t addend = *(content.data() + ri.r_offset - symbol->st_value);
        ERef->setAddend(addend);
        _references.push_back(ERef);
      }
    }

    // Create the DefinedAtom and add it to the list of DefinedAtoms.
    return new (_readerStorage) ELFDefinedAtom<ELFT>(
        *this, symbolName, sectionName, symbol, section, content,
        referenceStart, _references.size(), _references);
  }

  /// \brief After all the Atoms and References are created, update each
  /// Reference's target with the Atom pointer it refers to.
  void updateReferences() {
    /// cached value of target relocation handler
    assert(_targetHandler);
    const TargetRelocationHandler<ELFT> &targetRelocationHandler =
        _targetHandler->getRelocationHandler();

    for (auto &ri : _references) {
      if (ri->kindNamespace() == lld::Reference::KindNamespace::ELF) {
        const Elf_Sym *symbol = _objFile->getSymbol(ri->targetSymbolIndex());
        const Elf_Shdr *shdr = _objFile->getSection(symbol);

        // If the atom is not in mergeable string section, the target atom is
        // simply that atom.
        if (!isMergeableStringSection(shdr)) {
          ri->setTarget(findAtom(symbol));
          continue;
        }

        // If the target atom is mergeable string atom, the atom might have been
        // merged with other atom having the same contents. Try to find the
        // merged one if that's the case.
        int64_t relocAddend = targetRelocationHandler.relocAddend(*ri);
        uint64_t addend = ri->addend() + relocAddend;
        const MergeSectionKey ms(shdr, addend);
        auto msec = _mergedSectionMap.find(ms);
        if (msec != _mergedSectionMap.end()) {
          ri->setTarget(msec->second);
          continue;
        }

        // The target atom was not merged. Mergeable atoms are not in
        // _symbolToAtomMapping, so we cannot find it by calling findAtom(). We
        // instead call findMergeAtom().
        if (symbol->getType() != llvm::ELF::STT_SECTION)
          addend = symbol->st_value + addend;
        ELFMergeAtom<ELFT> *mergedAtom = findMergeAtom(shdr, addend);
        ri->setOffset(addend - mergedAtom->offset());
        ri->setAddend(0);
        ri->setTarget(mergedAtom);
      }
    }
  }

  /// \brief Return true if the symbol is corresponding to an architecture
  /// specific section. We will let the TargetHandler handle such atoms.
  inline bool isTargetSpecificAtom(const Elf_Shdr *shdr,
                                   const Elf_Sym *sym) {
    return ((shdr && (shdr->sh_flags & llvm::ELF::SHF_MASKPROC)) ||
            (sym->st_shndx >= llvm::ELF::SHN_LOPROC &&
             sym->st_shndx <= llvm::ELF::SHN_HIPROC));
  }

  /// \brief Do we want to ignore the section. Ignored sections are
  /// not processed to create atoms
  bool isIgnoredSection(const Elf_Shdr *section) {
    switch (section->sh_type) {
    case llvm::ELF::SHT_NOTE:
    case llvm::ELF::SHT_STRTAB:
    case llvm::ELF::SHT_SYMTAB:
    case llvm::ELF::SHT_SYMTAB_SHNDX:
      return true;
    default:
      break;
    }
    return false;
  }

  /// \brief Is the current section be treated as a mergeable string section.
  /// The contents of a mergeable string section are null-terminated strings.
  /// If the section have mergeable strings, the linker would need to split
  /// the section into multiple atoms and mark them mergeByContent.
  bool isMergeableStringSection(const Elf_Shdr *section) {
    if (_doStringsMerge && section) {
      int64_t sectionFlags = section->sh_flags;
      sectionFlags &= ~llvm::ELF::SHF_ALLOC;
      // Mergeable string sections have both SHF_MERGE and SHF_STRINGS flags
      // set. sh_entsize is the size of each character which is normally 1.
      if ((section->sh_entsize < 2) &&
          (sectionFlags == (llvm::ELF::SHF_MERGE | llvm::ELF::SHF_STRINGS))) {
        return true;
      }
    }
    return false;
  }

  /// \brief Returns a new anonymous atom whose size is equal to the
  /// section size. That atom will be used to represent the entire
  /// section that have no symbols.
  ELFDefinedAtom<ELFT> *createSectionAtom(const Elf_Shdr *section,
                                          StringRef sectionName,
                                          StringRef sectionContents) {
    Elf_Sym *sym = new (_readerStorage) Elf_Sym;
    sym->st_name = 0;
    sym->setBindingAndType(llvm::ELF::STB_LOCAL, llvm::ELF::STT_SECTION);
    sym->st_other = 0;
    sym->st_shndx = 0;
    sym->st_value = 0;
    sym->st_size = 0;
    ArrayRef<uint8_t> content((const uint8_t *)sectionContents.data(),
                              sectionContents.size());
    auto *newAtom = new (_readerStorage) ELFDefinedAtom<ELFT>(
        *this, "", sectionName, sym, section, content, 0, 0, _references);
    newAtom->setOrdinal(++_ordinal);
    return newAtom;
  }

  /// Returns true if the symbol is common symbol. A common symbol represents a
  /// tentive definition in C. It has name, size and alignment constraint, but
  /// actual storage has not yet been allocated. (The linker will allocate
  /// storage for them in the later pass after coalescing tentative symbols by
  /// name.)
  bool isCommonSymbol(const Elf_Sym *symbol) {
    // This method handles only architecture independent stuffs, and don't know
    // whether an architecture dependent section is for common symbols or
    // not. Let the TargetHandler to make a decision if that's the case.
    if (isTargetSpecificAtom(nullptr, symbol)) {
      assert(_targetHandler);
      TargetAtomHandler<ELFT> &atomHandler =
          _targetHandler->targetAtomHandler();
      return atomHandler.getType(symbol) == llvm::ELF::STT_COMMON;
    }
    return symbol->getType() == llvm::ELF::STT_COMMON ||
        symbol->st_shndx == llvm::ELF::SHN_COMMON;
  }

  /// Returns the symbol's content size. The nextSymbol should be null if the
  /// symbol is the last one in the section.
  uint64_t symbolContentSize(const Elf_Shdr *section, const Elf_Sym *symbol,
                             const Elf_Sym *nextSymbol) {
    // if this is the last symbol, take up the remaining data.
    return nextSymbol
        ? nextSymbol->st_value - symbol->st_value
        : section->sh_size - symbol->st_value;
  }

  void createEdge(ELFDefinedAtom<ELFT> *from, ELFDefinedAtom<ELFT> *to,
                  uint32_t edgeKind) {
    auto reference = new (_readerStorage) ELFReference<ELFT>(edgeKind);
    reference->setTarget(to);
    from->addReference(reference);
  }

  llvm::BumpPtrAllocator _readerStorage;
  std::unique_ptr<llvm::object::ELFFile<ELFT> > _objFile;
  atom_collection_vector<DefinedAtom> _definedAtoms;
  atom_collection_vector<UndefinedAtom> _undefinedAtoms;
  atom_collection_vector<SharedLibraryAtom> _sharedLibraryAtoms;
  atom_collection_vector<AbsoluteAtom> _absoluteAtoms;

  /// \brief _relocationAddendReferences and _relocationReferences contain the
  /// list of relocations references.  In ELF, if a section named, ".text" has
  /// relocations will also have a section named ".rel.text" or ".rela.text"
  /// which will hold the entries. -- .rel or .rela is prepended to create
  /// the SHT_REL(A) section name.
  std::unordered_map<
      StringRef,
      range<typename llvm::object::ELFFile<ELFT>::Elf_Rela_Iter> >
  _relocationAddendReferences;
  MergedSectionMapT _mergedSectionMap;
  std::unordered_map<
      StringRef,
      range<typename llvm::object::ELFFile<ELFT>::Elf_Rel_Iter> >
  _relocationReferences;
  std::vector<ELFReference<ELFT> *> _references;
  llvm::DenseMap<const Elf_Sym *, Atom *> _symbolToAtomMapping;

  /// \brief Atoms that are created for a section that has the merge property
  /// set
  MergeAtomsT _mergeAtoms;

  /// \brief the section and the symbols that are contained within it to create
  /// used to create atoms
  std::map<const Elf_Shdr *, std::vector<Elf_Sym_Iter>> _sectionSymbols;

  /// \brief Sections that have merge string property
  std::vector<const Elf_Shdr *> _mergeStringSections;

  int64_t _ordinal;

  /// \brief the cached options relevant while reading the ELF File
  bool _doStringsMerge;
  TargetHandler<ELFT> *_targetHandler;
};

/// \brief All atoms are owned by a File. To add linker specific atoms
/// the atoms need to be inserted to a file called (CRuntimeFile) which
/// are basically additional symbols required by libc and other runtime
/// libraries part of executing a program. This class provides support
/// for adding absolute symbols and undefined symbols
template <class ELFT> class CRuntimeFile : public ELFFile<ELFT> {
public:
  typedef llvm::object::Elf_Sym_Impl<ELFT> Elf_Sym;
  CRuntimeFile(const ELFLinkingContext &context, StringRef name = "C runtime")
      : ELFFile<ELFT>(name) {}

  /// \brief add a global absolute atom
  virtual Atom *addAbsoluteAtom(StringRef symbolName) {
    assert(!symbolName.empty() && "AbsoluteAtoms must have a name");
    Elf_Sym *symbol = new (_allocator) Elf_Sym;
    symbol->st_name = 0;
    symbol->st_value = 0;
    symbol->st_shndx = llvm::ELF::SHN_ABS;
    symbol->setBindingAndType(llvm::ELF::STB_GLOBAL, llvm::ELF::STT_OBJECT);
    symbol->st_other = llvm::ELF::STV_DEFAULT;
    symbol->st_size = 0;
    auto *newAtom =
        new (_allocator) ELFAbsoluteAtom<ELFT>(*this, symbolName, symbol, -1);
    _absoluteAtoms._atoms.push_back(newAtom);
    return newAtom;
  }

  /// \brief add an undefined atom
  virtual Atom *addUndefinedAtom(StringRef symbolName) {
    assert(!symbolName.empty() && "UndefinedAtoms must have a name");
    Elf_Sym *symbol = new (_allocator) Elf_Sym;
    symbol->st_name = 0;
    symbol->st_value = 0;
    symbol->st_shndx = llvm::ELF::SHN_UNDEF;
    symbol->st_other = llvm::ELF::STV_DEFAULT;
    symbol->st_size = 0;
    auto *newAtom =
        new (_allocator) ELFUndefinedAtom<ELFT>(*this, symbolName, symbol);
    _undefinedAtoms._atoms.push_back(newAtom);
    return newAtom;
  }

  virtual const File::atom_collection<DefinedAtom> &defined() const {
    return _definedAtoms;
  }

  virtual const File::atom_collection<UndefinedAtom> &undefined() const {
    return _undefinedAtoms;
  }

  virtual const File::atom_collection<SharedLibraryAtom> &
  sharedLibrary() const {
    return _sharedLibraryAtoms;
  }

  virtual const File::atom_collection<AbsoluteAtom> &absolute() const {
    return _absoluteAtoms;
  }

  // cannot add atoms to C Runtime file
  virtual void addAtom(const Atom &) {
    llvm_unreachable("cannot add atoms to Runtime files");
  }

protected:
  llvm::BumpPtrAllocator _allocator;
  File::atom_collection_vector<DefinedAtom> _definedAtoms;
  File::atom_collection_vector<UndefinedAtom> _undefinedAtoms;
  File::atom_collection_vector<SharedLibraryAtom> _sharedLibraryAtoms;
  File::atom_collection_vector<AbsoluteAtom> _absoluteAtoms;
};

} // end namespace elf
} // end namespace lld

#endif
