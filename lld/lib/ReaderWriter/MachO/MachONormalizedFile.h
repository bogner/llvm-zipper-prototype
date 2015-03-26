//===- lib/ReaderWriter/MachO/MachONormalizedFile.h -----------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

///
/// \file These data structures comprise the "normalized" view of
/// mach-o object files. The normalized view is an in-memory only data structure
/// which is always in native endianness and pointer size.
///
/// The normalized view easily converts to and from YAML using YAML I/O.
///
/// The normalized view converts to and from binary mach-o object files using
/// the writeBinary() and readBinary() functions.
///
/// The normalized view converts to and from lld::Atoms using the
/// normalizedToAtoms() and normalizedFromAtoms().
///
/// Overall, the conversion paths available look like:
///
///                 +---------------+
///                 | binary mach-o |
///                 +---------------+
///                        ^
///                        |
///                        v
///                  +------------+         +------+
///                  | normalized |   <->   | yaml |
///                  +------------+         +------+
///                        ^
///                        |
///                        v
///                    +-------+
///                    | Atoms |
///                    +-------+
///

#include "lld/Core/Error.h"
#include "lld/Core/LLVM.h"
#include "lld/ReaderWriter/MachOLinkingContext.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/ErrorOr.h"
#include "llvm/Support/MachO.h"
#include "llvm/Support/YAMLTraits.h"

#ifndef LLD_READER_WRITER_MACHO_NORMALIZE_FILE_H
#define LLD_READER_WRITER_MACHO_NORMALIZE_FILE_H

using llvm::BumpPtrAllocator;
using llvm::yaml::Hex64;
using llvm::yaml::Hex32;
using llvm::yaml::Hex16;
using llvm::yaml::Hex8;
using llvm::yaml::SequenceTraits;
using llvm::MachO::HeaderFileType;
using llvm::MachO::BindType;
using llvm::MachO::RebaseType;
using llvm::MachO::NListType;
using llvm::MachO::RelocationInfoType;
using llvm::MachO::SectionType;
using llvm::MachO::LoadCommandType;
using llvm::MachO::ExportSymbolKind;
using llvm::MachO::DataRegionType;

namespace lld {
namespace mach_o {
namespace normalized {


/// The real mach-o relocation record is 8-bytes on disk and is
/// encoded in one of two different bit-field patterns.  This
/// normalized form has the union of all possible fields.
struct Relocation {
  Relocation() : offset(0), scattered(false),
                 type(llvm::MachO::GENERIC_RELOC_VANILLA),
                 length(0), pcRel(false), isExtern(false), value(0),
                 symbol(0) { }

  Hex32               offset;
  bool                scattered;
  RelocationInfoType  type;
  uint8_t             length;
  bool                pcRel;
  bool                isExtern;
  Hex32               value;
  uint32_t            symbol;
};

/// A typedef so that YAML I/O can treat this vector as a sequence.
typedef std::vector<Relocation> Relocations;

/// A typedef so that YAML I/O can process the raw bytes in a section.
typedef std::vector<Hex8> ContentBytes;

/// A typedef so that YAML I/O can treat indirect symbols as a flow sequence.
typedef std::vector<uint32_t> IndirectSymbols;

/// A typedef so that YAML I/O can encode/decode section attributes.
LLVM_YAML_STRONG_TYPEDEF(uint32_t, SectionAttr)

/// Mach-O has a 32-bit and 64-bit section record.  This normalized form
/// can support either kind.
struct Section {
  Section() : type(llvm::MachO::S_REGULAR),
              attributes(0), alignment(0), address(0) { }

  StringRef       segmentName;
  StringRef       sectionName;
  SectionType     type;
  SectionAttr     attributes;
  PowerOf2        alignment;
  Hex64           address;
  ArrayRef<uint8_t> content;
  Relocations     relocations;
  IndirectSymbols indirectSymbols;
};


/// A typedef so that YAML I/O can encode/decode the scope bits of an nlist.
LLVM_YAML_STRONG_TYPEDEF(uint8_t, SymbolScope)

/// A typedef so that YAML I/O can encode/decode the desc bits of an nlist.
LLVM_YAML_STRONG_TYPEDEF(uint16_t, SymbolDesc)

/// Mach-O has a 32-bit and 64-bit symbol table entry (nlist), and the symbol
/// type and scope and mixed in the same n_type field.  This normalized form
/// works for any pointer size and separates out the type and scope.
struct Symbol {
  Symbol() : type(llvm::MachO::N_UNDF), scope(0), sect(0), desc(0), value(0) { }

  StringRef     name;
  NListType     type;
  SymbolScope   scope;
  uint8_t       sect;
  SymbolDesc    desc;
  Hex64         value;
};

/// A typedef so that YAML I/O can (de/en)code the protection bits of a segment.
LLVM_YAML_STRONG_TYPEDEF(uint32_t, VMProtect)

/// A typedef to hold verions X.Y.X packed into 32-bit xxxx.yy.zz
LLVM_YAML_STRONG_TYPEDEF(uint32_t, PackedVersion)

/// Segments are only used in normalized final linked images (not in relocatable
/// object files). They specify how a range of the file is loaded.
struct Segment {
  StringRef     name;
  Hex64         address;
  Hex64         size;
  VMProtect     access;
};

/// Only used in normalized final linked images to specify on which dylibs
/// it depends.
struct DependentDylib {
  StringRef       path;
  LoadCommandType kind;
  PackedVersion   compatVersion;
  PackedVersion   currentVersion;
};

/// A normalized rebasing entry.  Only used in normalized final linked images.
struct RebaseLocation {
  Hex32         segOffset;
  uint8_t       segIndex;
  RebaseType    kind;
};

/// A normalized binding entry.  Only used in normalized final linked images.
struct BindLocation {
  Hex32           segOffset;
  uint8_t         segIndex;
  BindType        kind;
  bool            canBeNull;
  int             ordinal;
  StringRef       symbolName;
  Hex64           addend;
};

/// A typedef so that YAML I/O can encode/decode export flags.
LLVM_YAML_STRONG_TYPEDEF(uint32_t, ExportFlags)

/// A normalized export entry.  Only used in normalized final linked images.
struct Export {
  StringRef         name;
  Hex64             offset;
  ExportSymbolKind  kind;
  ExportFlags       flags;
  Hex32             otherOffset;
  StringRef         otherName;
};

/// A normalized data-in-code entry.
struct DataInCode {
  Hex32           offset;
  Hex16           length;
  DataRegionType  kind;
};


/// A typedef so that YAML I/O can encode/decode mach_header.flags.
LLVM_YAML_STRONG_TYPEDEF(uint32_t, FileFlags)

///
struct NormalizedFile {
  NormalizedFile() : arch(MachOLinkingContext::arch_unknown),
                     fileType(llvm::MachO::MH_OBJECT),
                     flags(0),
                     hasUUID(false),
                     os(MachOLinkingContext::OS::unknown) { }

  MachOLinkingContext::Arch   arch;
  HeaderFileType              fileType;
  FileFlags                   flags;
  std::vector<Segment>        segments; // Not used in object files.
  std::vector<Section>        sections;

  // Symbols sorted by kind.
  std::vector<Symbol>         localSymbols;
  std::vector<Symbol>         globalSymbols;
  std::vector<Symbol>         undefinedSymbols;

  // Maps to load commands with no LINKEDIT content (final linked images only).
  std::vector<DependentDylib> dependentDylibs;
  StringRef                   installName;      // dylibs only
  PackedVersion               compatVersion;    // dylibs only
  PackedVersion               currentVersion;   // dylibs only
  bool                        hasUUID;
  std::vector<StringRef>      rpaths;
  Hex64                       entryAddress;
  MachOLinkingContext::OS     os;
  Hex64                       sourceVersion;
  PackedVersion               minOSverson;
  PackedVersion               sdkVersion;

  // Maps to load commands with LINKEDIT content (final linked images only).
  Hex32                       pageSize;
  std::vector<RebaseLocation> rebasingInfo;
  std::vector<BindLocation>   bindingInfo;
  std::vector<BindLocation>   weakBindingInfo;
  std::vector<BindLocation>   lazyBindingInfo;
  std::vector<Export>         exportInfo;
  std::vector<DataInCode>     dataInCode;

  // TODO:
  // code-signature
  // split-seg-info
  // function-starts

  // For any allocations in this struct which need to be owned by this struct.
  BumpPtrAllocator            ownedAllocations;
};

/// Tests if a file is a non-fat mach-o object file.
bool isThinObjectFile(StringRef path, MachOLinkingContext::Arch &arch);

/// If the buffer is a fat file with the request arch, then this function
/// returns true with 'offset' and 'size' set to location of the arch slice
/// within the buffer.  Otherwise returns false;
bool sliceFromFatFile(const MemoryBuffer &mb, MachOLinkingContext::Arch arch,
                       uint32_t &offset, uint32_t &size);

/// Reads a mach-o file and produces an in-memory normalized view.
ErrorOr<std::unique_ptr<NormalizedFile>>
readBinary(std::unique_ptr<MemoryBuffer> &mb,
           const MachOLinkingContext::Arch arch);

/// Takes in-memory normalized view and writes a mach-o object file.
std::error_code writeBinary(const NormalizedFile &file, StringRef path);

size_t headerAndLoadCommandsSize(const NormalizedFile &file);


/// Parses a yaml encoded mach-o file to produce an in-memory normalized view.
ErrorOr<std::unique_ptr<NormalizedFile>>
readYaml(std::unique_ptr<MemoryBuffer> &mb);

/// Writes a yaml encoded mach-o files given an in-memory normalized view.
std::error_code writeYaml(const NormalizedFile &file, raw_ostream &out);

std::error_code
normalizedObjectToAtoms(MachOFile *file,
                        const NormalizedFile &normalizedFile,
                        bool copyRefs);

std::error_code
normalizedDylibToAtoms(MachODylibFile *file,
                       const NormalizedFile &normalizedFile,
                       bool copyRefs);

/// Takes in-memory normalized dylib or object and parses it into lld::File
ErrorOr<std::unique_ptr<lld::File>>
normalizedToAtoms(const NormalizedFile &normalizedFile, StringRef path,
                  bool copyRefs);

/// Takes atoms and generates a normalized macho-o view.
ErrorOr<std::unique_ptr<NormalizedFile>>
normalizedFromAtoms(const lld::File &atomFile, const MachOLinkingContext &ctxt);


} // namespace normalized

/// Class for interfacing mach-o yaml files into generic yaml parsing
class MachOYamlIOTaggedDocumentHandler : public YamlIOTaggedDocumentHandler {
public:
  MachOYamlIOTaggedDocumentHandler(MachOLinkingContext::Arch arch)
    : _arch(arch) { }
  bool handledDocTag(llvm::yaml::IO &io, const lld::File *&file) const override;
private:
  const MachOLinkingContext::Arch _arch;
};

} // namespace mach_o
} // namespace lld

#endif // LLD_READER_WRITER_MACHO_NORMALIZE_FILE_H
