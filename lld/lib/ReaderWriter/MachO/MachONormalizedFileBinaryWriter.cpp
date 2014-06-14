//===- lib/ReaderWriter/MachO/MachONormalizedFileBinaryWriter.cpp ---------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

///
/// \file For mach-o object files, this implementation converts normalized
/// mach-o in memory to mach-o binary on disk.
///
///                 +---------------+
///                 | binary mach-o |
///                 +---------------+
///                        ^
///                        |
///                        |
///                  +------------+
///                  | normalized |
///                  +------------+

#include "MachONormalizedFile.h"
#include "MachONormalizedFileBinaryUtils.h"
#include "lld/Core/Error.h"
#include "lld/Core/LLVM.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FileOutputBuffer.h"
#include "llvm/Support/Host.h"
#include "llvm/Support/LEB128.h"
#include "llvm/Support/MachO.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"
#include <functional>
#include <map>
#include <system_error>

using namespace llvm::MachO;

namespace lld {
namespace mach_o {
namespace normalized {

/// Utility class for writing a mach-o binary file given an in-memory
/// normalized file.
class MachOFileLayout {
public:
  /// All layout computation is done in the constructor.
  MachOFileLayout(const NormalizedFile &file);

  /// Returns the final file size as computed in the constructor.
  size_t      size() const;

  /// Writes the normalized file as a binary mach-o file to the specified
  /// path.  This does not have a stream interface because the generated
  /// file may need the 'x' bit set.
  std::error_code writeBinary(StringRef path);

private:
  uint32_t    loadCommandsSize(uint32_t &count);
  void        buildFileOffsets();
  void        writeMachHeader();
  std::error_code writeLoadCommands();
  void        writeSectionContent();
  void        writeRelocations();
  void        writeSymbolTable();
  void        writeRebaseInfo();
  void        writeBindingInfo();
  void        writeLazyBindingInfo();
  void        writeLinkEditContent();
  void        buildLinkEditInfo();
  void        buildRebaseInfo();
  void        buildBindInfo();
  void        buildLazyBindInfo();
  void        computeSymbolTableSizes();
  void        buildSectionRelocations();
  void        appendSymbols(const std::vector<Symbol> &symbols,
                                      uint32_t &symOffset, uint32_t &strOffset);
  uint32_t    indirectSymbolIndex(const Section &sect, uint32_t &index);
  uint32_t    indirectSymbolElementSize(const Section &sect);

  // For use as template parameter to load command methods.
  struct MachO64Trait {
    typedef llvm::MachO::segment_command_64 command;
    typedef llvm::MachO::section_64         section;
    enum { LC = llvm::MachO::LC_SEGMENT_64 };
  };

  // For use as template parameter to load command methods.
  struct MachO32Trait {
    typedef llvm::MachO::segment_command   command;
    typedef llvm::MachO::section           section;
    enum { LC = llvm::MachO::LC_SEGMENT };
  };

  template <typename T>
  std::error_code writeSingleSegmentLoadCommand(uint8_t *&lc);
  template <typename T> std::error_code writeSegmentLoadCommands(uint8_t *&lc);

  uint32_t pointerAlign(uint32_t value);
  static StringRef dyldPath();

  class ByteBuffer {
  public:
    ByteBuffer() : _ostream(_bytes) { }
    void append_byte(uint8_t b) {
      _ostream << b;
    }
    void append_uleb128(uint64_t value) {
      llvm::encodeULEB128(value, _ostream);
    }
    void append_sleb128(int64_t value) {
      llvm::encodeSLEB128(value, _ostream);
    }
    void append_string(StringRef str) {
      _ostream << str;
      append_byte(0);
    }
    void align(unsigned alignment) {
      while ( (_ostream.tell() % alignment) != 0 )
        append_byte(0);
    }
    size_t size() {
      return _ostream.tell();
    }
    const uint8_t *bytes() {
      return reinterpret_cast<const uint8_t*>(_ostream.str().data());
    }
  private:
    SmallVector<char, 128>        _bytes;
    // Stream ivar must be after SmallVector ivar to construct properly.
    llvm::raw_svector_ostream     _ostream;
  };


  struct SegExtraInfo {
    uint32_t                    fileOffset;
    std::vector<const Section*> sections;
  };
  typedef std::map<const Segment*, SegExtraInfo> SegMap;
  struct SectionExtraInfo {
    uint32_t                    fileOffset;
  };
  typedef std::map<const Section*, SectionExtraInfo> SectionMap;

  const NormalizedFile &_file;
  std::error_code _ec;
  uint8_t              *_buffer;
  const bool            _is64;
  const bool            _swap;
  const bool            _bigEndianArch;
  uint64_t              _seg1addr;
  uint32_t              _startOfLoadCommands;
  uint32_t              _countOfLoadCommands;
  uint32_t              _endOfLoadCommands;
  uint32_t              _startOfRelocations;
  uint32_t              _startOfSymbols;
  uint32_t              _startOfIndirectSymbols;
  uint32_t              _startOfSymbolStrings;
  uint32_t              _endOfSymbolStrings;
  uint32_t              _symbolTableLocalsStartIndex;
  uint32_t              _symbolTableGlobalsStartIndex;
  uint32_t              _symbolTableUndefinesStartIndex;
  uint32_t              _symbolStringPoolSize;
  uint32_t              _symbolTableSize;
  uint32_t              _indirectSymbolTableCount;
  // Used in object file creation only
  uint32_t              _startOfSectionsContent;
  uint32_t              _endOfSectionsContent;
  // Used in final linked image only
  uint32_t              _startOfLinkEdit;
  uint32_t              _startOfRebaseInfo;
  uint32_t              _endOfRebaseInfo;
  uint32_t              _startOfBindingInfo;
  uint32_t              _endOfBindingInfo;
  uint32_t              _startOfLazyBindingInfo;
  uint32_t              _endOfLazyBindingInfo;
  uint32_t              _endOfLinkEdit;
  uint64_t              _addressOfLinkEdit;
  SegMap                _segInfo;
  SectionMap            _sectInfo;
  ByteBuffer            _rebaseInfo;
  ByteBuffer            _bindingInfo;
  ByteBuffer            _lazyBindingInfo;
  ByteBuffer            _weakBindingInfo;
  ByteBuffer            _exportInfo;
};

size_t headerAndLoadCommandsSize(const NormalizedFile &file) {
  MachOFileLayout layout(file);
  return layout.size();
}

StringRef MachOFileLayout::dyldPath() {
  return "/usr/lib/dyld";
}

uint32_t MachOFileLayout::pointerAlign(uint32_t value) {
  return llvm::RoundUpToAlignment(value, _is64 ? 8 : 4);
}




MachOFileLayout::MachOFileLayout(const NormalizedFile &file)
    : _file(file),
      _is64(MachOLinkingContext::is64Bit(file.arch)),
      _swap(!MachOLinkingContext::isHostEndian(file.arch)),
      _bigEndianArch(MachOLinkingContext::isBigEndian(file.arch)),
      _seg1addr(INT64_MAX) {
  _startOfLoadCommands = _is64 ? sizeof(mach_header_64) : sizeof(mach_header);
  const size_t segCommandBaseSize =
          (_is64 ? sizeof(segment_command_64) : sizeof(segment_command));
  const size_t sectsSize = (_is64 ? sizeof(section_64) : sizeof(section));
  if (file.fileType == llvm::MachO::MH_OBJECT) {
    // object files have just one segment load command containing all sections
    _endOfLoadCommands = _startOfLoadCommands
                               + segCommandBaseSize
                               + file.sections.size() * sectsSize
                               + sizeof(symtab_command);
    _countOfLoadCommands = 2;

    // Accumulate size of each section.
    _startOfSectionsContent = _endOfLoadCommands;
    _endOfSectionsContent = _startOfSectionsContent;
    unsigned relocCount = 0;
    for (const Section &sect : file.sections) {
      _sectInfo[&sect].fileOffset = _endOfSectionsContent;
      _endOfSectionsContent += sect.content.size();
      relocCount += sect.relocations.size();
    }

    computeSymbolTableSizes();

    // Align start of relocations.
    _startOfRelocations = pointerAlign(_endOfSectionsContent);
    _startOfSymbols = _startOfRelocations + relocCount * 8;
    // Add Indirect symbol table.
    _startOfIndirectSymbols = _startOfSymbols + _symbolTableSize;
    // Align start of symbol table and symbol strings.
    _startOfSymbolStrings = _startOfIndirectSymbols
                  + pointerAlign(_indirectSymbolTableCount * sizeof(uint32_t));
    _endOfSymbolStrings = _startOfSymbolStrings
                          + pointerAlign(_symbolStringPoolSize);
    _endOfLinkEdit = _endOfSymbolStrings;
    DEBUG_WITH_TYPE("MachOFileLayout",
                  llvm::dbgs() << "MachOFileLayout()\n"
      << "  startOfLoadCommands=" << _startOfLoadCommands << "\n"
      << "  countOfLoadCommands=" << _countOfLoadCommands << "\n"
      << "  endOfLoadCommands=" << _endOfLoadCommands << "\n"
      << "  startOfRelocations=" << _startOfRelocations << "\n"
      << "  startOfSymbols=" << _startOfSymbols << "\n"
      << "  startOfSymbolStrings=" << _startOfSymbolStrings << "\n"
      << "  endOfSymbolStrings=" << _endOfSymbolStrings << "\n"
      << "  startOfSectionsContent=" << _startOfSectionsContent << "\n"
      << "  endOfSectionsContent=" << _endOfSectionsContent << "\n");
  } else {
    // Final linked images have one load command per segment.
    _endOfLoadCommands = _startOfLoadCommands
                          + loadCommandsSize(_countOfLoadCommands);

    // Assign section file offsets.
    buildFileOffsets();
    buildLinkEditInfo();

    // LINKEDIT of final linked images has in order:
    // rebase info, binding info, lazy binding info, weak binding info,
    // indirect symbol table, symbol table, symbol table strings.
    _startOfRebaseInfo = _startOfLinkEdit;
    _endOfRebaseInfo = _startOfRebaseInfo + _rebaseInfo.size();
    _startOfBindingInfo = _endOfRebaseInfo;
    _endOfBindingInfo = _startOfBindingInfo + _bindingInfo.size();
    _startOfLazyBindingInfo = _endOfBindingInfo;
    _endOfLazyBindingInfo = _startOfLazyBindingInfo + _lazyBindingInfo.size();

    _startOfSymbols = _endOfLazyBindingInfo;
    _startOfIndirectSymbols = _startOfSymbols + _symbolTableSize;
    _startOfSymbolStrings = _startOfIndirectSymbols
                  + pointerAlign(_indirectSymbolTableCount * sizeof(uint32_t));
    _endOfSymbolStrings = _startOfSymbolStrings
                          + pointerAlign(_symbolStringPoolSize);
    _endOfLinkEdit = _endOfSymbolStrings;
    DEBUG_WITH_TYPE("MachOFileLayout",
                  llvm::dbgs() << "MachOFileLayout()\n"
      << "  startOfLoadCommands=" << _startOfLoadCommands << "\n"
      << "  countOfLoadCommands=" << _countOfLoadCommands << "\n"
      << "  endOfLoadCommands=" << _endOfLoadCommands << "\n"
      << "  startOfLinkEdit=" << _startOfLinkEdit << "\n"
      << "  startOfRebaseInfo=" << _startOfRebaseInfo << "\n"
      << "  endOfRebaseInfo=" << _endOfRebaseInfo << "\n"
      << "  startOfBindingInfo=" << _startOfBindingInfo << "\n"
      << "  endOfBindingInfo=" << _endOfBindingInfo << "\n"
      << "  startOfLazyBindingInfo=" << _startOfLazyBindingInfo << "\n"
      << "  endOfLazyBindingInfo=" << _endOfLazyBindingInfo << "\n"
      << "  startOfSymbols=" << _startOfSymbols << "\n"
      << "  startOfSymbolStrings=" << _startOfSymbolStrings << "\n"
      << "  endOfSymbolStrings=" << _endOfSymbolStrings << "\n"
      << "  addressOfLinkEdit=" << _addressOfLinkEdit << "\n");
  }
}

uint32_t MachOFileLayout::loadCommandsSize(uint32_t &count) {
  uint32_t size = 0;
  count = 0;

  const size_t segCommandSize =
          (_is64 ? sizeof(segment_command_64) : sizeof(segment_command));
  const size_t sectionSize = (_is64 ? sizeof(section_64) : sizeof(section));

  // Add LC_SEGMENT for each segment.
  size += _file.segments.size() * segCommandSize;
  count += _file.segments.size();
  // Add section record for each section.
  size += _file.sections.size() * sectionSize;
  // Add one LC_SEGMENT for implicit  __LINKEDIT segment
  size += segCommandSize;
  ++count;

  // Add LC_DYLD_INFO
  size += sizeof(dyld_info_command);
  ++count;

  // Add LC_SYMTAB
  size += sizeof(symtab_command);
  ++count;

  // Add LC_DYSYMTAB
  if (_file.fileType != llvm::MachO::MH_PRELOAD) {
    size += sizeof(dysymtab_command);
    ++count;
  }

  // If main executable add LC_LOAD_DYLINKER and LC_MAIN
  if (_file.fileType == llvm::MachO::MH_EXECUTE) {
    size += pointerAlign(sizeof(dylinker_command) + dyldPath().size()+1);
    ++count;
    size += sizeof(entry_point_command);
    ++count;
  }

  // Add LC_LOAD_DYLIB for each dependent dylib.
  for (const DependentDylib &dep : _file.dependentDylibs) {
    size += sizeof(dylib_command) + pointerAlign(dep.path.size()+1);
    ++count;
  }

  return size;
}

static bool overlaps(const Segment &s1, const Segment &s2) {
  if (s2.address >= s1.address+s1.size)
    return false;
  if (s1.address >= s2.address+s2.size)
    return false;
  return true;
}

static bool overlaps(const Section &s1, const Section &s2) {
  if (s2.address >= s1.address+s1.content.size())
    return false;
  if (s1.address >= s2.address+s2.content.size())
    return false;
  return true;
}

void MachOFileLayout::buildFileOffsets() {
  // Verify no segments overlap
  for (const Segment &sg1 : _file.segments) {
    for (const Segment &sg2 : _file.segments) {
      if (&sg1 == &sg2)
        continue;
      if (overlaps(sg1,sg2)) {
        _ec = make_error_code(llvm::errc::executable_format_error);
        return;
      }
    }
  }

  // Verify no sections overlap
  for (const Section &s1 : _file.sections) {
    for (const Section &s2 : _file.sections) {
      if (&s1 == &s2)
        continue;
      if (overlaps(s1,s2)) {
        _ec = make_error_code(llvm::errc::executable_format_error);
        return;
      }
    }
  }

  // Build side table of extra info about segments and sections.
  SegExtraInfo t;
  t.fileOffset = 0;
  for (const Segment &sg : _file.segments) {
    _segInfo[&sg] = t;
  }
  SectionExtraInfo t2;
  t2.fileOffset = 0;
  // Assign sections to segments.
  for (const Section &s : _file.sections) {
    _sectInfo[&s] = t2;
    for (const Segment &sg : _file.segments) {
      if ((s.address >= sg.address)
                        && (s.address+s.content.size() <= sg.address+sg.size)) {
        if (!sg.name.equals(s.segmentName)) {
          _ec = make_error_code(llvm::errc::executable_format_error);
          return;
        }
        _segInfo[&sg].sections.push_back(&s);
      }
    }
  }

  // Assign file offsets.
  uint32_t fileOffset = 0;
  DEBUG_WITH_TYPE("MachOFileLayout",
                  llvm::dbgs() << "buildFileOffsets()\n");
  for (const Segment &sg : _file.segments) {
    // FIXME: 4096 should be inferred from segments in normalized file.
    _segInfo[&sg].fileOffset = llvm::RoundUpToAlignment(fileOffset, 4096);
    if ((_seg1addr == INT64_MAX) && sg.access)
      _seg1addr = sg.address;
    DEBUG_WITH_TYPE("MachOFileLayout",
                  llvm::dbgs() << "  segment=" << sg.name
                  << ", fileOffset=" << _segInfo[&sg].fileOffset << "\n");
    for (const Section *s : _segInfo[&sg].sections) {
      fileOffset = s->address - sg.address + _segInfo[&sg].fileOffset;
      _sectInfo[s].fileOffset = fileOffset;
      DEBUG_WITH_TYPE("MachOFileLayout",
                  llvm::dbgs() << "    section=" << s->sectionName
                  << ", fileOffset=" << fileOffset << "\n");
    }
    _addressOfLinkEdit = sg.address + sg.size;
  }
  _startOfLinkEdit = llvm::RoundUpToAlignment(fileOffset, 4096);
}


size_t MachOFileLayout::size() const {
  return _endOfSymbolStrings;
}

void MachOFileLayout::writeMachHeader() {
  mach_header *mh = reinterpret_cast<mach_header*>(_buffer);
  mh->magic = _is64 ? llvm::MachO::MH_MAGIC_64 : llvm::MachO::MH_MAGIC;
  mh->cputype =  MachOLinkingContext::cpuTypeFromArch(_file.arch);
  mh->cpusubtype = MachOLinkingContext::cpuSubtypeFromArch(_file.arch);
  mh->filetype = _file.fileType;
  mh->ncmds = _countOfLoadCommands;
  mh->sizeofcmds = _endOfLoadCommands - _startOfLoadCommands;
  mh->flags = _file.flags;
  if (_swap)
    swapStruct(*mh);
}

uint32_t MachOFileLayout::indirectSymbolIndex(const Section &sect,
                                                   uint32_t &index) {
  if (sect.indirectSymbols.empty())
    return 0;
  uint32_t result = index;
  index += sect.indirectSymbols.size();
  return result;
}

uint32_t MachOFileLayout::indirectSymbolElementSize(const Section &sect) {
  if (sect.indirectSymbols.empty())
    return 0;
  if (sect.type != S_SYMBOL_STUBS)
    return 0;
  return sect.content.size() / sect.indirectSymbols.size();
}

template <typename T>
std::error_code MachOFileLayout::writeSingleSegmentLoadCommand(uint8_t *&lc) {
  typename T::command* seg = reinterpret_cast<typename T::command*>(lc);
  seg->cmd = T::LC;
  seg->cmdsize = sizeof(typename T::command)
                          + _file.sections.size() * sizeof(typename T::section);
  uint8_t *next = lc + seg->cmdsize;
  memset(seg->segname, 0, 16);
  seg->vmaddr = 0;
  seg->vmsize = _endOfSectionsContent - _endOfLoadCommands;
  seg->fileoff = _endOfLoadCommands;
  seg->filesize = seg->vmsize;
  seg->maxprot = VM_PROT_READ|VM_PROT_WRITE|VM_PROT_EXECUTE;
  seg->initprot = VM_PROT_READ|VM_PROT_WRITE|VM_PROT_EXECUTE;
  seg->nsects = _file.sections.size();
  seg->flags = 0;
  if (_swap)
    swapStruct(*seg);
  typename T::section *sout = reinterpret_cast<typename T::section*>
                                              (lc+sizeof(typename T::command));
  uint32_t relOffset = _startOfRelocations;
  uint32_t contentOffset = _startOfSectionsContent;
  uint32_t indirectSymRunningIndex = 0;
  for (const Section &sin : _file.sections) {
    setString16(sin.sectionName, sout->sectname);
    setString16(sin.segmentName, sout->segname);
    sout->addr = sin.address;
    sout->size = sin.content.size();
    sout->offset = contentOffset;
    sout->align = sin.alignment;
    sout->reloff = sin.relocations.empty() ? 0 : relOffset;
    sout->nreloc = sin.relocations.size();
    sout->flags = sin.type | sin.attributes;
    sout->reserved1 = indirectSymbolIndex(sin, indirectSymRunningIndex);
    sout->reserved2 = indirectSymbolElementSize(sin);
    relOffset += sin.relocations.size() * sizeof(any_relocation_info);
    contentOffset += sin.content.size();
    if (_swap)
      swapStruct(*sout);
    ++sout;
  }
  lc = next;
  return std::error_code();
}

template <typename T>
std::error_code MachOFileLayout::writeSegmentLoadCommands(uint8_t *&lc) {
  uint32_t indirectSymRunningIndex = 0;
  for (const Segment &seg : _file.segments) {
    // Write segment command with trailing sections.
    SegExtraInfo &segInfo = _segInfo[&seg];
    typename T::command* cmd = reinterpret_cast<typename T::command*>(lc);
    cmd->cmd = T::LC;
    cmd->cmdsize = sizeof(typename T::command)
                        + segInfo.sections.size() * sizeof(typename T::section);
    uint8_t *next = lc + cmd->cmdsize;
    setString16(seg.name, cmd->segname);
    cmd->vmaddr   = seg.address;
    cmd->vmsize   = seg.size;
    cmd->fileoff  = segInfo.fileOffset;
    cmd->filesize = seg.access ? seg.size : Hex64(0);
    cmd->maxprot  = seg.access;
    cmd->initprot = seg.access;
    cmd->nsects   = segInfo.sections.size();
    cmd->flags    = 0;
    if (_swap)
      swapStruct(*cmd);
    typename T::section *sect = reinterpret_cast<typename T::section*>
                                               (lc+sizeof(typename T::command));
    for (const Section *section : segInfo.sections) {
      setString16(section->sectionName, sect->sectname);
      setString16(section->segmentName, sect->segname);
      sect->addr      = section->address;
      sect->size      = section->content.size();
      sect->offset    = section->address - seg.address + segInfo.fileOffset;
      sect->align     = section->alignment;
      sect->reloff    = 0;
      sect->nreloc    = 0;
      sect->flags     = section->type | section->attributes;
      sect->reserved1 = indirectSymbolIndex(*section, indirectSymRunningIndex);
      sect->reserved2 = indirectSymbolElementSize(*section);
      if (_swap)
        swapStruct(*sect);
      ++sect;
    }
    lc = reinterpret_cast<uint8_t*>(next);
  }
  // Add implicit __LINKEDIT segment
  typename T::command* cmd = reinterpret_cast<typename T::command*>(lc);
  cmd->cmd = T::LC;
  cmd->cmdsize = sizeof(typename T::command);
  uint8_t *next = lc + cmd->cmdsize;
  setString16("__LINKEDIT", cmd->segname);
  cmd->vmaddr   = _addressOfLinkEdit;
  cmd->vmsize   = _endOfLinkEdit - _startOfLinkEdit;
  cmd->fileoff  = _startOfLinkEdit;
  cmd->filesize = _endOfLinkEdit - _startOfLinkEdit;
  cmd->maxprot  = VM_PROT_READ;
  cmd->initprot = VM_PROT_READ;
  cmd->nsects   = 0;
  cmd->flags    = 0;
  if (_swap)
    swapStruct(*cmd);
  lc = next;
  return std::error_code();
}

std::error_code MachOFileLayout::writeLoadCommands() {
  std::error_code ec;
  uint8_t *lc = &_buffer[_startOfLoadCommands];
  if (_file.fileType == llvm::MachO::MH_OBJECT) {
    // Object files have one unnamed segment which holds all sections.
    if (_is64)
      ec = writeSingleSegmentLoadCommand<MachO64Trait>(lc);
    else
      ec = writeSingleSegmentLoadCommand<MachO32Trait>(lc);
    // Add LC_SYMTAB with symbol table info
    symtab_command* st = reinterpret_cast<symtab_command*>(lc);
    st->cmd     = LC_SYMTAB;
    st->cmdsize = sizeof(symtab_command);
    st->symoff  = _startOfSymbols;
    st->nsyms   = _file.localSymbols.size() + _file.globalSymbols.size()
                                            + _file.undefinedSymbols.size();
    st->stroff  = _startOfSymbolStrings;
    st->strsize = _endOfSymbolStrings - _startOfSymbolStrings;
    if (_swap)
      swapStruct(*st);
  } else {
    // Final linked images have sections under segments.
    if (_is64)
      ec = writeSegmentLoadCommands<MachO64Trait>(lc);
    else
      ec = writeSegmentLoadCommands<MachO32Trait>(lc);

    // Add LC_DYLD_INFO_ONLY.
    dyld_info_command* di = reinterpret_cast<dyld_info_command*>(lc);
    di->cmd            = LC_DYLD_INFO_ONLY;
    di->cmdsize        = sizeof(dyld_info_command);
    di->rebase_off     = _rebaseInfo.size() ? _startOfRebaseInfo : 0;
    di->rebase_size    = _rebaseInfo.size();
    di->bind_off       = _bindingInfo.size() ? _startOfBindingInfo : 0;
    di->bind_size      = _bindingInfo.size();
    di->weak_bind_off  = 0;
    di->weak_bind_size = 0;
    di->lazy_bind_off  = _lazyBindingInfo.size() ? _startOfLazyBindingInfo : 0;
    di->lazy_bind_size = _lazyBindingInfo.size();
    di->export_off     = 0;
    di->export_size    = 0;
    if (_swap)
      swapStruct(*di);
    lc += sizeof(dyld_info_command);

    // Add LC_SYMTAB with symbol table info.
    symtab_command* st = reinterpret_cast<symtab_command*>(lc);
    st->cmd     = LC_SYMTAB;
    st->cmdsize = sizeof(symtab_command);
    st->symoff  = _startOfSymbols;
    st->nsyms   = _file.localSymbols.size() + _file.globalSymbols.size()
                                            + _file.undefinedSymbols.size();
    st->stroff  = _startOfSymbolStrings;
    st->strsize = _endOfSymbolStrings - _startOfSymbolStrings;
    if (_swap)
      swapStruct(*st);
    lc += sizeof(symtab_command);

    // Add LC_DYSYMTAB
    if (_file.fileType != llvm::MachO::MH_PRELOAD) {
      dysymtab_command* dst = reinterpret_cast<dysymtab_command*>(lc);
      dst->cmd            = LC_DYSYMTAB;
      dst->cmdsize        = sizeof(dysymtab_command);
      dst->ilocalsym      = _symbolTableLocalsStartIndex;
      dst->nlocalsym      = _file.localSymbols.size();
      dst->iextdefsym     = _symbolTableGlobalsStartIndex;
      dst->nextdefsym     = _file.globalSymbols.size();
      dst->iundefsym      = _symbolTableUndefinesStartIndex;
      dst->nundefsym      = _file.undefinedSymbols.size();
      dst->tocoff         = 0;
      dst->ntoc           = 0;
      dst->modtaboff      = 0;
      dst->nmodtab        = 0;
      dst->extrefsymoff   = 0;
      dst->nextrefsyms    = 0;
      dst->indirectsymoff = _startOfIndirectSymbols;
      dst->nindirectsyms  = _indirectSymbolTableCount;
      dst->extreloff      = 0;
      dst->nextrel        = 0;
      dst->locreloff      = 0;
      dst->nlocrel        = 0;
      if (_swap)
        swapStruct(*dst);
      lc += sizeof(dysymtab_command);
    }

    // If main executable, add LC_LOAD_DYLINKER and LC_MAIN.
    if (_file.fileType == llvm::MachO::MH_EXECUTE) {
      // Build LC_LOAD_DYLINKER load command.
      uint32_t size=pointerAlign(sizeof(dylinker_command)+dyldPath().size()+1);
      dylinker_command* dl = reinterpret_cast<dylinker_command*>(lc);
      dl->cmd              = LC_LOAD_DYLINKER;
      dl->cmdsize          = size;
      dl->name             = sizeof(dylinker_command); // offset
      if (_swap)
        swapStruct(*dl);
      memcpy(lc+sizeof(dylinker_command), dyldPath().data(), dyldPath().size());
      lc[sizeof(dylinker_command)+dyldPath().size()] = '\0';
      lc += size;
      // Build LC_MAIN load command.
      entry_point_command* ep = reinterpret_cast<entry_point_command*>(lc);
      ep->cmd       = LC_MAIN;
      ep->cmdsize   = sizeof(entry_point_command);
      ep->entryoff  = _file.entryAddress - _seg1addr;
      ep->stacksize = 0;
      if (_swap)
        swapStruct(*ep);
      lc += sizeof(entry_point_command);
    }

    // Add LC_LOAD_DYLIB commands
    for (const DependentDylib &dep : _file.dependentDylibs) {
      dylib_command* dc = reinterpret_cast<dylib_command*>(lc);
      uint32_t size = sizeof(dylib_command) + pointerAlign(dep.path.size()+1);
      dc->cmd                         = LC_LOAD_DYLIB;
      dc->cmdsize                     = size;
      dc->dylib.name                  = sizeof(dylib_command); // offset
      dc->dylib.timestamp             = 0; // FIXME
      dc->dylib.current_version       = 0; // FIXME
      dc->dylib.compatibility_version = 0; // FIXME
      if (_swap)
        swapStruct(*dc);
      memcpy(lc+sizeof(dylib_command), dep.path.begin(), dep.path.size());
      lc[sizeof(dylib_command)+dep.path.size()] = '\0';
      lc += size;
    }

  }
  return ec;
}


void MachOFileLayout::writeSectionContent() {
  for (const Section &s : _file.sections) {
    // Copy all section content to output buffer.
    if (s.type == llvm::MachO::S_ZEROFILL)
      continue;
    uint32_t offset = _sectInfo[&s].fileOffset;
    uint8_t *p = &_buffer[offset];
    memcpy(p, &s.content[0], s.content.size());
    p += s.content.size();
  }
}

void MachOFileLayout::writeRelocations() {
  uint32_t relOffset = _startOfRelocations;
  for (Section sect : _file.sections) {
    for (Relocation r : sect.relocations) {
      any_relocation_info* rb = reinterpret_cast<any_relocation_info*>(
                                                           &_buffer[relOffset]);
      *rb = packRelocation(r, _swap, _bigEndianArch);
      relOffset += sizeof(any_relocation_info);
    }
  }
}


void MachOFileLayout::appendSymbols(const std::vector<Symbol> &symbols,
                                   uint32_t &symOffset, uint32_t &strOffset) {
  for (const Symbol &sym : symbols) {
    if (_is64) {
      nlist_64* nb = reinterpret_cast<nlist_64*>(&_buffer[symOffset]);
      nb->n_strx = strOffset - _startOfSymbolStrings;
      nb->n_type = sym.type | sym.scope;
      nb->n_sect = sym.sect;
      nb->n_desc = sym.desc;
      nb->n_value = sym.value;
      if (_swap)
        swapStruct(*nb);
      symOffset += sizeof(nlist_64);
    } else {
      nlist* nb = reinterpret_cast<nlist*>(&_buffer[symOffset]);
      nb->n_strx = strOffset - _startOfSymbolStrings;
      nb->n_type = sym.type | sym.scope;
      nb->n_sect = sym.sect;
      nb->n_desc = sym.desc;
      nb->n_value = sym.value;
      if (_swap)
        swapStruct(*nb);
      symOffset += sizeof(nlist);
    }
    memcpy(&_buffer[strOffset], sym.name.begin(), sym.name.size());
    strOffset += sym.name.size();
    _buffer[strOffset++] ='\0'; // Strings in table have nul terminator.
  }
}

void MachOFileLayout::writeSymbolTable() {
  // Write symbol table and symbol strings in parallel.
  uint32_t symOffset = _startOfSymbols;
  uint32_t strOffset = _startOfSymbolStrings;
  _buffer[strOffset++] = '\0'; // Reserve n_strx offset of zero to mean no name.
  appendSymbols(_file.localSymbols, symOffset, strOffset);
  appendSymbols(_file.globalSymbols, symOffset, strOffset);
  appendSymbols(_file.undefinedSymbols, symOffset, strOffset);
  // Write indirect symbol table array.
  uint32_t *indirects = reinterpret_cast<uint32_t*>
                                            (&_buffer[_startOfIndirectSymbols]);
  if (_file.fileType == llvm::MachO::MH_OBJECT) {
    // Object files have sections in same order as input normalized file.
    for (const Section &section : _file.sections) {
      for (uint32_t index : section.indirectSymbols) {
        if (_swap)
          *indirects++ = llvm::sys::getSwappedBytes(index);
        else
          *indirects++ = index;
      }
    }
  } else {
    // Final linked images must sort sections from normalized file.
    for (const Segment &seg : _file.segments) {
      SegExtraInfo &segInfo = _segInfo[&seg];
      for (const Section *section : segInfo.sections) {
        for (uint32_t index : section->indirectSymbols) {
          if (_swap)
            *indirects++ = llvm::sys::getSwappedBytes(index);
          else
            *indirects++ = index;
        }
      }
    }
  }
}

void MachOFileLayout::writeRebaseInfo() {
  memcpy(&_buffer[_startOfRebaseInfo], _rebaseInfo.bytes(), _rebaseInfo.size());
}

void MachOFileLayout::writeBindingInfo() {
  memcpy(&_buffer[_startOfBindingInfo],
                                    _bindingInfo.bytes(), _bindingInfo.size());
}

void MachOFileLayout::writeLazyBindingInfo() {
  memcpy(&_buffer[_startOfLazyBindingInfo],
                            _lazyBindingInfo.bytes(), _lazyBindingInfo.size());
}

void MachOFileLayout::buildLinkEditInfo() {
  buildRebaseInfo();
  buildBindInfo();
  buildLazyBindInfo();
  computeSymbolTableSizes();
}

void MachOFileLayout::buildSectionRelocations() {

}

void MachOFileLayout::buildRebaseInfo() {
  // TODO: compress rebasing info.
  for (const RebaseLocation& entry : _file.rebasingInfo) {
    _rebaseInfo.append_byte(REBASE_OPCODE_SET_TYPE_IMM | entry.kind);
    _rebaseInfo.append_byte(REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB
                            | entry.segIndex);
    _rebaseInfo.append_uleb128(entry.segOffset);
    _rebaseInfo.append_uleb128(REBASE_OPCODE_DO_REBASE_IMM_TIMES | 1);
  }
  _rebaseInfo.append_byte(REBASE_OPCODE_DONE);
  _rebaseInfo.align(_is64 ? 8 : 4);
}

void MachOFileLayout::buildBindInfo() {
  // TODO: compress bind info.
  for (const BindLocation& entry : _file.bindingInfo) {
    _bindingInfo.append_byte(BIND_OPCODE_SET_TYPE_IMM | entry.kind);
    _bindingInfo.append_byte(BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB
                            | entry.segIndex);
    _bindingInfo.append_uleb128(entry.segOffset);
    _bindingInfo.append_byte(BIND_OPCODE_SET_DYLIB_ORDINAL_IMM | entry.ordinal);
    _bindingInfo.append_byte(BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM);
    _bindingInfo.append_string(entry.symbolName);
    if (entry.addend != 0) {
      _bindingInfo.append_byte(BIND_OPCODE_SET_ADDEND_SLEB);
      _bindingInfo.append_sleb128(entry.addend);
    }
    _bindingInfo.append_byte(BIND_OPCODE_DO_BIND);
  }
  _bindingInfo.append_byte(BIND_OPCODE_DONE);
  _bindingInfo.align(_is64 ? 8 : 4);
}

void MachOFileLayout::buildLazyBindInfo() {
  for (const BindLocation& entry : _file.lazyBindingInfo) {
    _lazyBindingInfo.append_byte(BIND_OPCODE_SET_TYPE_IMM | entry.kind);
    _lazyBindingInfo.append_byte(BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB
                            | entry.segIndex);
    _lazyBindingInfo.append_uleb128(entry.segOffset);
    _lazyBindingInfo.append_byte(BIND_OPCODE_SET_DYLIB_ORDINAL_IMM | entry.ordinal);
    _lazyBindingInfo.append_byte(BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM);
    _lazyBindingInfo.append_string(entry.symbolName);
    _lazyBindingInfo.append_byte(BIND_OPCODE_DO_BIND);
  }
  _lazyBindingInfo.append_byte(BIND_OPCODE_DONE);
  _lazyBindingInfo.align(_is64 ? 8 : 4);
}

void MachOFileLayout::computeSymbolTableSizes() {
  // MachO symbol tables have three ranges: locals, globals, and undefines
  const size_t nlistSize = (_is64 ? sizeof(nlist_64) : sizeof(nlist));
  _symbolTableSize = nlistSize * (_file.localSymbols.size()
                                + _file.globalSymbols.size()
                                + _file.undefinedSymbols.size());
  _symbolStringPoolSize = 0;
  for (const Symbol &sym : _file.localSymbols) {
    _symbolStringPoolSize += (sym.name.size()+1);
  }
  for (const Symbol &sym : _file.globalSymbols) {
    _symbolStringPoolSize += (sym.name.size()+1);
  }
  for (const Symbol &sym : _file.undefinedSymbols) {
    _symbolStringPoolSize += (sym.name.size()+1);
  }
  _symbolTableLocalsStartIndex = 0;
  _symbolTableGlobalsStartIndex = _file.localSymbols.size();
  _symbolTableUndefinesStartIndex = _symbolTableGlobalsStartIndex
                                    + _file.globalSymbols.size();

  _indirectSymbolTableCount = 0;
  for (const Section &sect : _file.sections) {
    _indirectSymbolTableCount += sect.indirectSymbols.size();
  }
}


void MachOFileLayout::writeLinkEditContent() {
  if (_file.fileType == llvm::MachO::MH_OBJECT) {
    writeRelocations();
    writeSymbolTable();
  } else {
    writeRebaseInfo();
    writeBindingInfo();
    writeLazyBindingInfo();
    // TODO: add weak binding info
    writeSymbolTable();
  }
}

std::error_code MachOFileLayout::writeBinary(StringRef path) {
  // Check for pending error from constructor.
  if (_ec)
    return _ec;
  // Create FileOutputBuffer with calculated size.
  std::unique_ptr<llvm::FileOutputBuffer> fob;
  unsigned flags = 0;
  if (_file.fileType != llvm::MachO::MH_OBJECT)
    flags = llvm::FileOutputBuffer::F_executable;
  std::error_code ec;
  ec = llvm::FileOutputBuffer::create(path, size(), fob, flags);
  if (ec)
    return ec;

  // Write content.
  _buffer = fob->getBufferStart();
  writeMachHeader();
  ec = writeLoadCommands();
  if (ec)
    return ec;
  writeSectionContent();
  writeLinkEditContent();
  fob->commit();

  return std::error_code();
}



/// Takes in-memory normalized view and writes a mach-o object file.
std::error_code writeBinary(const NormalizedFile &file, StringRef path) {
  MachOFileLayout layout(file);
  return layout.writeBinary(path);
}


} // namespace normalized
} // namespace mach_o
} // namespace lld

