//===- InputFiles.h ---------------------------------------------*- C++ -*-===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_ELF_INPUT_FILES_H
#define LLD_ELF_INPUT_FILES_H

#include "Config.h"
#include "InputSection.h"
#include "Error.h"
#include "Symbols.h"

#include "lld/Core/LLVM.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/Comdat.h"
#include "llvm/Object/Archive.h"
#include "llvm/Object/ELF.h"
#include "llvm/Object/IRObjectFile.h"
#include "llvm/Support/StringSaver.h"

namespace lld {
namespace elf {

using llvm::object::Archive;

class InputFile;
class Lazy;
class SymbolBody;

// The root class of input files.
class InputFile {
public:
  enum Kind { ObjectKind, SharedKind, ArchiveKind, BitcodeKind };
  Kind kind() const { return FileKind; }

  StringRef getName() const { return MB.getBufferIdentifier(); }
  MemoryBufferRef MB;

  // Filename of .a which contained this file. If this file was
  // not in an archive file, it is the empty string. We use this
  // string for creating error messages.
  StringRef ArchiveName;

protected:
  InputFile(Kind K, MemoryBufferRef M) : MB(M), FileKind(K) {}

private:
  const Kind FileKind;
};

template <typename ELFT> class ELFFileBase : public InputFile {
public:
  typedef typename ELFT::Shdr Elf_Shdr;
  typedef typename ELFT::Sym Elf_Sym;
  typedef typename ELFT::Word Elf_Word;
  typedef typename ELFT::SymRange Elf_Sym_Range;

  ELFFileBase(Kind K, MemoryBufferRef M);
  static bool classof(const InputFile *F) {
    Kind K = F->kind();
    return K == ObjectKind || K == SharedKind;
  }

  static ELFKind getELFKind();
  const llvm::object::ELFFile<ELFT> &getObj() const { return ELFObj; }
  llvm::object::ELFFile<ELFT> &getObj() { return ELFObj; }

  uint16_t getEMachine() const { return getObj().getHeader()->e_machine; }
  uint8_t getOSABI() const {
    return getObj().getHeader()->e_ident[llvm::ELF::EI_OSABI];
  }

  StringRef getStringTable() const { return StringTable; }

  uint32_t getSectionIndex(const Elf_Sym &Sym) const;

protected:
  llvm::object::ELFFile<ELFT> ELFObj;
  const Elf_Shdr *Symtab = nullptr;
  ArrayRef<Elf_Word> SymtabSHNDX;
  StringRef StringTable;
  void initStringTable();
  Elf_Sym_Range getElfSymbols(bool OnlyGlobals);
};

// .o file.
template <class ELFT> class ObjectFile : public ELFFileBase<ELFT> {
  typedef ELFFileBase<ELFT> Base;
  typedef typename ELFT::Sym Elf_Sym;
  typedef typename ELFT::Shdr Elf_Shdr;
  typedef typename ELFT::SymRange Elf_Sym_Range;
  typedef typename ELFT::Word Elf_Word;
  typedef typename ELFT::uint uintX_t;

  StringRef getShtGroupSignature(const Elf_Shdr &Sec);
  ArrayRef<Elf_Word> getShtGroupEntries(const Elf_Shdr &Sec);

public:
  static bool classof(const InputFile *F) {
    return F->kind() == Base::ObjectKind;
  }

  ArrayRef<SymbolBody *> getSymbols();
  ArrayRef<SymbolBody *> getLocalSymbols();
  ArrayRef<SymbolBody *> getNonLocalSymbols();

  explicit ObjectFile(MemoryBufferRef M);
  void parse(llvm::DenseSet<StringRef> &ComdatGroups);

  ArrayRef<InputSectionBase<ELFT> *> getSections() const { return Sections; }
  InputSectionBase<ELFT> *getSection(const Elf_Sym &Sym) const;

  SymbolBody &getSymbolBody(uint32_t SymbolIndex) const {
    return *SymbolBodies[SymbolIndex];
  }

  const Elf_Shdr *getSymbolTable() const { return this->Symtab; };

  // Get MIPS GP0 value defined by this file. This value represents the gp value
  // used to create the relocatable object and required to support
  // R_MIPS_GPREL16 / R_MIPS_GPREL32 relocations.
  uint32_t getMipsGp0() const;

  // The number is the offset in the string table. It will be used as the
  // st_name of the symbol.
  std::vector<std::pair<const DefinedRegular<ELFT> *, unsigned>> KeptLocalSyms;

private:
  void initializeSections(llvm::DenseSet<StringRef> &ComdatGroups);
  void initializeSymbols();
  InputSectionBase<ELFT> *getRelocTarget(const Elf_Shdr &Sec);
  InputSectionBase<ELFT> *createInputSection(const Elf_Shdr &Sec);

  SymbolBody *createSymbolBody(const Elf_Sym *Sym);

  // List of all sections defined by this file.
  std::vector<InputSectionBase<ELFT> *> Sections;

  // List of all symbols referenced or defined by this file.
  std::vector<SymbolBody *> SymbolBodies;

  // MIPS .reginfo section defined by this file.
  MipsReginfoInputSection<ELFT> *MipsReginfo = nullptr;

  llvm::BumpPtrAllocator Alloc;
  llvm::SpecificBumpPtrAllocator<MergeInputSection<ELFT>> MAlloc;
  llvm::SpecificBumpPtrAllocator<EHInputSection<ELFT>> EHAlloc;
};

class ArchiveFile : public InputFile {
public:
  explicit ArchiveFile(MemoryBufferRef M) : InputFile(ArchiveKind, M) {}
  static bool classof(const InputFile *F) { return F->kind() == ArchiveKind; }
  void parse();

  // Returns a memory buffer for a given symbol. An empty memory buffer
  // is returned if we have already returned the same memory buffer.
  // (So that we don't instantiate same members more than once.)
  MemoryBufferRef getMember(const Archive::Symbol *Sym);

  llvm::MutableArrayRef<Lazy> getLazySymbols() { return LazySymbols; }

private:
  std::unique_ptr<Archive> File;
  std::vector<Lazy> LazySymbols;
  llvm::DenseSet<uint64_t> Seen;
};

class BitcodeFile : public InputFile {
public:
  explicit BitcodeFile(MemoryBufferRef M);
  static bool classof(const InputFile *F);
  void parse(llvm::DenseSet<StringRef> &ComdatGroups);
  ArrayRef<SymbolBody *> getSymbols() { return SymbolBodies; }
  static bool shouldSkip(const llvm::object::BasicSymbolRef &Sym);

private:
  std::vector<SymbolBody *> SymbolBodies;
  llvm::BumpPtrAllocator Alloc;
  llvm::StringSaver Saver{Alloc};
  SymbolBody *
  createSymbolBody(const llvm::DenseSet<const llvm::Comdat *> &KeptComdats,
                   const llvm::object::IRObjectFile &Obj,
                   const llvm::object::BasicSymbolRef &Sym);
};

// .so file.
template <class ELFT> class SharedFile : public ELFFileBase<ELFT> {
  typedef ELFFileBase<ELFT> Base;
  typedef typename ELFT::Shdr Elf_Shdr;
  typedef typename ELFT::Sym Elf_Sym;
  typedef typename ELFT::Word Elf_Word;
  typedef typename ELFT::SymRange Elf_Sym_Range;

  std::vector<SharedSymbol<ELFT>> SymbolBodies;
  std::vector<StringRef> Undefs;
  StringRef SoName;

public:
  StringRef getSoName() const { return SoName; }
  llvm::MutableArrayRef<SharedSymbol<ELFT>> getSharedSymbols() {
    return SymbolBodies;
  }
  const Elf_Shdr *getSection(const Elf_Sym &Sym) const;
  llvm::ArrayRef<StringRef> getUndefinedSymbols() { return Undefs; }

  static bool classof(const InputFile *F) {
    return F->kind() == Base::SharedKind;
  }

  explicit SharedFile(MemoryBufferRef M);

  void parseSoName();
  void parseRest();

  // Used for --as-needed
  bool AsNeeded = false;
  bool IsUsed = false;
  bool isNeeded() const { return !AsNeeded || IsUsed; }
};

std::unique_ptr<InputFile> createObjectFile(MemoryBufferRef MB,
                                            StringRef ArchiveName = "");
std::unique_ptr<InputFile> createSharedFile(MemoryBufferRef MB);

} // namespace elf
} // namespace lld

#endif
