//===- lib/ReaderWriter/PECOFF/PECOFFLinkingContext.cpp -------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Atoms.h"
#include "EdataPass.h"
#include "GroupedSectionsPass.h"
#include "IdataPass.h"
#include "LinkerGeneratedSymbolFile.h"
#include "SetSubsystemPass.h"

#include "lld/Core/PassManager.h"
#include "lld/Passes/LayoutPass.h"
#include "lld/Passes/RoundTripNativePass.h"
#include "lld/Passes/RoundTripYAMLPass.h"
#include "lld/ReaderWriter/PECOFFLinkingContext.h"
#include "lld/ReaderWriter/Reader.h"
#include "lld/ReaderWriter/Simple.h"
#include "lld/ReaderWriter/Writer.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/Path.h"

#include <bitset>
#include <climits>
#include <set>

namespace lld {

static void assignOrdinals(PECOFFLinkingContext &ctx) {
  int maxOrdinal = -1;
  for (const PECOFFLinkingContext::ExportDesc &desc : ctx.getDllExports())
    maxOrdinal = std::max(maxOrdinal, desc.ordinal);
  int nextOrdinal = (maxOrdinal == -1) ? 1 : (maxOrdinal + 1);
  for (PECOFFLinkingContext::ExportDesc &desc : ctx.getDllExports())
    if (desc.ordinal == -1)
      desc.ordinal = nextOrdinal++;
}

bool PECOFFLinkingContext::validateImpl(raw_ostream &diagnostics) {
  if (_stackReserve < _stackCommit) {
    diagnostics << "Invalid stack size: reserve size must be equal to or "
                << "greater than commit size, but got " << _stackCommit
                << " and " << _stackReserve << ".\n";
    return false;
  }

  if (_heapReserve < _heapCommit) {
    diagnostics << "Invalid heap size: reserve size must be equal to or "
                << "greater than commit size, but got " << _heapCommit
                << " and " << _heapReserve << ".\n";
    return false;
  }

  // It's an error if the base address is not multiple of 64K.
  if (_baseAddress & 0xffff) {
    diagnostics << "Base address have to be multiple of 64K, but got "
                << _baseAddress << "\n";
    return false;
  }

  // Check for duplicate export ordinals.
  std::set<int> exports;
  for (const PECOFFLinkingContext::ExportDesc &desc : getDllExports()) {
    if (desc.ordinal == -1)
      continue;
    if (exports.count(desc.ordinal) == 1) {
      diagnostics << "Duplicate export ordinals: " << desc.ordinal << "\n";
      return false;
    }
    exports.insert(desc.ordinal);
  }

  std::bitset<64> alignment(_sectionDefaultAlignment);
  if (alignment.count() != 1) {
    diagnostics << "Section alignment must be a power of 2, but got "
                << _sectionDefaultAlignment << "\n";
    return false;
  }

  // Architectures other than i386 is not supported yet.
  if (_machineType != llvm::COFF::IMAGE_FILE_MACHINE_I386) {
    diagnostics << "Machine type other than x86 is not supported.\n";
    return false;
  }

  // Assign default ordinals to export symbols.
  assignOrdinals(*this);

  _writer = createWriterPECOFF(*this);
  return true;
}

std::unique_ptr<File> PECOFFLinkingContext::createEntrySymbolFile() const {
  if (entrySymbolName().empty())
    return nullptr;
  std::unique_ptr<SimpleFile> entryFile(
      new SimpleFile("command line option /entry"));
  entryFile->addAtom(
      *(new (_allocator) SimpleUndefinedAtom(*entryFile, entrySymbolName())));
  return std::move(entryFile);
}

std::unique_ptr<File> PECOFFLinkingContext::createUndefinedSymbolFile() const {
  if (_initialUndefinedSymbols.empty())
    return nullptr;
  std::unique_ptr<SimpleFile> undefinedSymFile(
      new SimpleFile("command line option /c (or) /include"));
  for (auto undefSymStr : _initialUndefinedSymbols)
    undefinedSymFile->addAtom(*(new (_allocator) SimpleUndefinedAtom(
                                   *undefinedSymFile, undefSymStr)));
  return std::move(undefinedSymFile);
}

bool PECOFFLinkingContext::createImplicitFiles(
    std::vector<std::unique_ptr<File> > &) const {
  std::unique_ptr<SimpleFileNode> fileNode(
      new SimpleFileNode("Implicit Files"));
  std::unique_ptr<File> linkerGeneratedSymFile(
      new pecoff::LinkerGeneratedSymbolFile(*this));
  fileNode->appendInputFile(std::move(linkerGeneratedSymFile));
  inputGraph().insertOneElementAt(std::move(fileNode),
                                  InputGraph::Position::END);
  return true;
}

/// Returns the section name in the resulting executable.
///
/// Sections in object files are usually output to the executable with the same
/// name, but you can rename by command line option. /merge:from=to makes the
/// linker to combine "from" section contents to "to" section in the
/// executable. We have a mapping for the renaming. This method looks up the
/// table and returns a new section name if renamed.
StringRef
PECOFFLinkingContext::getOutputSectionName(StringRef sectionName) const {
  auto it = _renamedSections.find(sectionName);
  if (it == _renamedSections.end())
    return sectionName;
  return getOutputSectionName(it->second);
}

/// Adds a mapping to the section renaming table. This method will be used for
/// /merge command line option.
bool PECOFFLinkingContext::addSectionRenaming(raw_ostream &diagnostics,
                                              StringRef from, StringRef to) {
  auto it = _renamedSections.find(from);
  if (it != _renamedSections.end()) {
    if (it->second == to)
      // There's already the same mapping.
      return true;
    diagnostics << "Section \"" << from << "\" is already mapped to \""
                << it->second << ", so it cannot be mapped to \"" << to << "\".";
    return true;
  }

  // Add a mapping, and check if there's no cycle in the renaming mapping. The
  // cycle detection algorithm we use here is naive, but that's OK because the
  // number of mapping is usually less than 10.
  _renamedSections[from] = to;
  for (auto elem : _renamedSections) {
    StringRef sectionName = elem.first;
    std::set<StringRef> visited;
    visited.insert(sectionName);
    for (;;) {
      auto pos = _renamedSections.find(sectionName);
      if (pos == _renamedSections.end())
        break;
      if (visited.count(pos->second)) {
        diagnostics << "/merge:" << from << "=" << to << " makes a cycle";
        return false;
      }
      sectionName = pos->second;
      visited.insert(sectionName);
    }
  }
  return true;
}

StringRef PECOFFLinkingContext::getAlternateName(StringRef def) const {
  auto it = _alternateNames.find(def);
  if (it == _alternateNames.end())
    return "";
  return it->second;
}

void PECOFFLinkingContext::setAlternateName(StringRef weak, StringRef def) {
  _alternateNames[def] = weak;
}

/// Try to find the input library file from the search paths and append it to
/// the input file list. Returns true if the library file is found.
StringRef PECOFFLinkingContext::searchLibraryFile(StringRef filename) const {
  // Current directory always takes precedence over the search paths.
  if (llvm::sys::path::is_absolute(filename) || llvm::sys::fs::exists(filename))
    return filename;
  // Iterate over the search paths.
  for (StringRef dir : _inputSearchPaths) {
    SmallString<128> path = dir;
    llvm::sys::path::append(path, filename);
    if (llvm::sys::fs::exists(path.str()))
      return allocate(path.str());
  }
  return filename;
}

/// Returns the decorated name of the given symbol name. On 32-bit x86, it
/// adds "_" at the beginning of the string. On other architectures, the
/// return value is the same as the argument.
StringRef PECOFFLinkingContext::decorateSymbol(StringRef name) const {
  if (_machineType != llvm::COFF::IMAGE_FILE_MACHINE_I386)
    return name;
  std::string str = "_";
  str.append(name);
  return allocate(str);
}

StringRef PECOFFLinkingContext::undecorateSymbol(StringRef name) const {
  if (_machineType != llvm::COFF::IMAGE_FILE_MACHINE_I386)
    return name;
  assert(name.startswith("_"));
  return name.substr(1);
}

Writer &PECOFFLinkingContext::writer() const { return *_writer; }


void PECOFFLinkingContext::setSectionSetMask(StringRef sectionName,
                                             uint32_t newFlags) {
  _sectionSetMask[sectionName] |= newFlags;
  _sectionClearMask[sectionName] &= ~newFlags;
  const uint32_t rwx = (llvm::COFF::IMAGE_SCN_MEM_READ |
                        llvm::COFF::IMAGE_SCN_MEM_WRITE |
                        llvm::COFF::IMAGE_SCN_MEM_EXECUTE);
  if (newFlags & rwx)
    _sectionClearMask[sectionName] |= ~_sectionSetMask[sectionName] & rwx;
  assert((_sectionSetMask[sectionName] & _sectionClearMask[sectionName]) == 0);
}

void PECOFFLinkingContext::setSectionClearMask(StringRef sectionName,
                                               uint32_t newFlags) {
  _sectionClearMask[sectionName] |= newFlags;
  _sectionSetMask[sectionName] &= ~newFlags;
  assert((_sectionSetMask[sectionName] & _sectionClearMask[sectionName]) == 0);
}

uint32_t PECOFFLinkingContext::getSectionAttributes(StringRef sectionName,
                                                    uint32_t flags) const {
  auto si = _sectionSetMask.find(sectionName);
  uint32_t setMask = (si == _sectionSetMask.end()) ? 0 : si->second;
  auto ci = _sectionClearMask.find(sectionName);
  uint32_t clearMask = (ci == _sectionClearMask.end()) ? 0 : ci->second;
  return (flags | setMask) & ~clearMask;
}

void PECOFFLinkingContext::addDllExport(ExportDesc &desc) {
  if (_dllExportSet.count(desc.name)) {
    llvm::errs() << "Export symbol '" << desc.name
                 << "' specified more than once.";
    return;
  }
  _dllExports.push_back(desc);
  _dllExportSet.insert(desc.name);
}

void PECOFFLinkingContext::addPasses(PassManager &pm) {
  pm.add(std::unique_ptr<Pass>(new pecoff::SetSubsystemPass(*this)));
  pm.add(std::unique_ptr<Pass>(new pecoff::EdataPass(*this)));
  pm.add(std::unique_ptr<Pass>(new pecoff::IdataPass(*this)));
  pm.add(std::unique_ptr<Pass>(new LayoutPass()));
  pm.add(std::unique_ptr<Pass>(new pecoff::GroupedSectionsPass()));
}

} // end namespace lld
