//===-- SymbolFileNativePDB.cpp ---------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "SymbolFileNativePDB.h"

#include "clang/AST/Attr.h"
#include "clang/AST/CharUnits.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/Type.h"

#include "lldb/Core/Module.h"
#include "lldb/Core/PluginManager.h"
#include "lldb/Core/StreamBuffer.h"
#include "lldb/Core/StreamFile.h"
#include "lldb/Symbol/ClangASTContext.h"
#include "lldb/Symbol/ClangASTImporter.h"
#include "lldb/Symbol/ClangExternalASTSourceCommon.h"
#include "lldb/Symbol/ClangUtil.h"
#include "lldb/Symbol/CompileUnit.h"
#include "lldb/Symbol/LineTable.h"
#include "lldb/Symbol/ObjectFile.h"
#include "lldb/Symbol/SymbolContext.h"
#include "lldb/Symbol/SymbolVendor.h"
#include "lldb/Symbol/Variable.h"
#include "lldb/Symbol/VariableList.h"

#include "llvm/DebugInfo/CodeView/CVRecord.h"
#include "llvm/DebugInfo/CodeView/CVTypeVisitor.h"
#include "llvm/DebugInfo/CodeView/DebugLinesSubsection.h"
#include "llvm/DebugInfo/CodeView/LazyRandomTypeCollection.h"
#include "llvm/DebugInfo/CodeView/RecordName.h"
#include "llvm/DebugInfo/CodeView/SymbolDeserializer.h"
#include "llvm/DebugInfo/CodeView/TypeDeserializer.h"
#include "llvm/DebugInfo/PDB/Native/DbiStream.h"
#include "llvm/DebugInfo/PDB/Native/GlobalsStream.h"
#include "llvm/DebugInfo/PDB/Native/InfoStream.h"
#include "llvm/DebugInfo/PDB/Native/ModuleDebugStream.h"
#include "llvm/DebugInfo/PDB/Native/PDBFile.h"
#include "llvm/DebugInfo/PDB/Native/SymbolStream.h"
#include "llvm/DebugInfo/PDB/Native/TpiStream.h"
#include "llvm/DebugInfo/PDB/PDBTypes.h"
#include "llvm/Demangle/MicrosoftDemangle.h"
#include "llvm/Object/COFF.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/BinaryStreamReader.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/ErrorOr.h"
#include "llvm/Support/MemoryBuffer.h"

#include "PdbSymUid.h"
#include "PdbUtil.h"
#include "UdtRecordCompleter.h"

using namespace lldb;
using namespace lldb_private;
using namespace npdb;
using namespace llvm::codeview;
using namespace llvm::pdb;

static lldb::LanguageType TranslateLanguage(PDB_Lang lang) {
  switch (lang) {
  case PDB_Lang::Cpp:
    return lldb::LanguageType::eLanguageTypeC_plus_plus;
  case PDB_Lang::C:
    return lldb::LanguageType::eLanguageTypeC;
  default:
    return lldb::LanguageType::eLanguageTypeUnknown;
  }
}

static std::unique_ptr<PDBFile> loadPDBFile(std::string PdbPath,
                                            llvm::BumpPtrAllocator &Allocator) {
  llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> ErrorOrBuffer =
      llvm::MemoryBuffer::getFile(PdbPath, /*FileSize=*/-1,
                                  /*RequiresNullTerminator=*/false);
  if (!ErrorOrBuffer)
    return nullptr;
  std::unique_ptr<llvm::MemoryBuffer> Buffer = std::move(*ErrorOrBuffer);

  llvm::StringRef Path = Buffer->getBufferIdentifier();
  auto Stream = llvm::make_unique<llvm::MemoryBufferByteStream>(
      std::move(Buffer), llvm::support::little);

  auto File = llvm::make_unique<PDBFile>(Path, std::move(Stream), Allocator);
  if (auto EC = File->parseFileHeaders()) {
    llvm::consumeError(std::move(EC));
    return nullptr;
  }
  if (auto EC = File->parseStreamData()) {
    llvm::consumeError(std::move(EC));
    return nullptr;
  }

  return File;
}

static std::unique_ptr<PDBFile>
loadMatchingPDBFile(std::string exe_path, llvm::BumpPtrAllocator &allocator) {
  // Try to find a matching PDB for an EXE.
  using namespace llvm::object;
  auto expected_binary = createBinary(exe_path);

  // If the file isn't a PE/COFF executable, fail.
  if (!expected_binary) {
    llvm::consumeError(expected_binary.takeError());
    return nullptr;
  }
  OwningBinary<Binary> binary = std::move(*expected_binary);

  auto *obj = llvm::dyn_cast<llvm::object::COFFObjectFile>(binary.getBinary());
  if (!obj)
    return nullptr;
  const llvm::codeview::DebugInfo *pdb_info = nullptr;

  // If it doesn't have a debug directory, fail.
  llvm::StringRef pdb_file;
  auto ec = obj->getDebugPDBInfo(pdb_info, pdb_file);
  if (ec)
    return nullptr;

  // if the file doesn't exist, is not a pdb, or doesn't have a matching guid,
  // fail.
  llvm::file_magic magic;
  ec = llvm::identify_magic(pdb_file, magic);
  if (ec || magic != llvm::file_magic::pdb)
    return nullptr;
  std::unique_ptr<PDBFile> pdb = loadPDBFile(pdb_file, allocator);
  if (!pdb)
    return nullptr;

  auto expected_info = pdb->getPDBInfoStream();
  if (!expected_info) {
    llvm::consumeError(expected_info.takeError());
    return nullptr;
  }
  llvm::codeview::GUID guid;
  memcpy(&guid, pdb_info->PDB70.Signature, 16);

  if (expected_info->getGuid() != guid)
    return nullptr;
  return pdb;
}

static bool IsFunctionPrologue(const CompilandIndexItem &cci,
                               lldb::addr_t addr) {
  // FIXME: Implement this.
  return false;
}

static bool IsFunctionEpilogue(const CompilandIndexItem &cci,
                               lldb::addr_t addr) {
  // FIXME: Implement this.
  return false;
}

static clang::MSInheritanceAttr::Spelling
GetMSInheritance(LazyRandomTypeCollection &tpi, const ClassRecord &record) {
  if (record.DerivationList == TypeIndex::None())
    return clang::MSInheritanceAttr::Spelling::Keyword_single_inheritance;

  CVType bases = tpi.getType(record.DerivationList);
  ArgListRecord base_list;
  cantFail(TypeDeserializer::deserializeAs<ArgListRecord>(bases, base_list));
  if (base_list.ArgIndices.empty())
    return clang::MSInheritanceAttr::Spelling::Keyword_single_inheritance;

  int base_count = 0;
  for (TypeIndex ti : base_list.ArgIndices) {
    CVType base = tpi.getType(ti);
    if (base.kind() == LF_VBCLASS || base.kind() == LF_IVBCLASS)
      return clang::MSInheritanceAttr::Spelling::Keyword_virtual_inheritance;
    ++base_count;
  }

  if (base_count > 1)
    return clang::MSInheritanceAttr::Keyword_multiple_inheritance;
  return clang::MSInheritanceAttr::Keyword_single_inheritance;
}

static lldb::BasicType GetCompilerTypeForSimpleKind(SimpleTypeKind kind) {
  switch (kind) {
  case SimpleTypeKind::Boolean128:
  case SimpleTypeKind::Boolean16:
  case SimpleTypeKind::Boolean32:
  case SimpleTypeKind::Boolean64:
  case SimpleTypeKind::Boolean8:
    return lldb::eBasicTypeBool;
  case SimpleTypeKind::Byte:
  case SimpleTypeKind::UnsignedCharacter:
    return lldb::eBasicTypeUnsignedChar;
  case SimpleTypeKind::NarrowCharacter:
    return lldb::eBasicTypeChar;
  case SimpleTypeKind::SignedCharacter:
  case SimpleTypeKind::SByte:
    return lldb::eBasicTypeSignedChar;
  case SimpleTypeKind::Character16:
    return lldb::eBasicTypeChar16;
  case SimpleTypeKind::Character32:
    return lldb::eBasicTypeChar32;
  case SimpleTypeKind::Complex80:
    return lldb::eBasicTypeLongDoubleComplex;
  case SimpleTypeKind::Complex64:
    return lldb::eBasicTypeDoubleComplex;
  case SimpleTypeKind::Complex32:
    return lldb::eBasicTypeFloatComplex;
  case SimpleTypeKind::Float128:
  case SimpleTypeKind::Float80:
    return lldb::eBasicTypeLongDouble;
  case SimpleTypeKind::Float64:
    return lldb::eBasicTypeDouble;
  case SimpleTypeKind::Float32:
    return lldb::eBasicTypeFloat;
  case SimpleTypeKind::Float16:
    return lldb::eBasicTypeHalf;
  case SimpleTypeKind::Int128:
    return lldb::eBasicTypeInt128;
  case SimpleTypeKind::Int64:
  case SimpleTypeKind::Int64Quad:
    return lldb::eBasicTypeLongLong;
  case SimpleTypeKind::Int32:
    return lldb::eBasicTypeInt;
  case SimpleTypeKind::Int16:
  case SimpleTypeKind::Int16Short:
    return lldb::eBasicTypeShort;
  case SimpleTypeKind::UInt128:
    return lldb::eBasicTypeUnsignedInt128;
  case SimpleTypeKind::UInt64:
  case SimpleTypeKind::UInt64Quad:
    return lldb::eBasicTypeUnsignedLongLong;
  case SimpleTypeKind::HResult:
  case SimpleTypeKind::UInt32:
    return lldb::eBasicTypeUnsignedInt;
  case SimpleTypeKind::UInt16:
  case SimpleTypeKind::UInt16Short:
    return lldb::eBasicTypeUnsignedShort;
  case SimpleTypeKind::Int32Long:
    return lldb::eBasicTypeLong;
  case SimpleTypeKind::UInt32Long:
    return lldb::eBasicTypeUnsignedLong;
  case SimpleTypeKind::Void:
    return lldb::eBasicTypeVoid;
  case SimpleTypeKind::WideCharacter:
    return lldb::eBasicTypeWChar;
  default:
    return lldb::eBasicTypeInvalid;
  }
}

static bool IsSimpleTypeSignedInteger(SimpleTypeKind kind) {
  switch (kind) {
  case SimpleTypeKind::Int128:
  case SimpleTypeKind::Int64:
  case SimpleTypeKind::Int64Quad:
  case SimpleTypeKind::Int32:
  case SimpleTypeKind::Int32Long:
  case SimpleTypeKind::Int16:
  case SimpleTypeKind::Int16Short:
  case SimpleTypeKind::Float128:
  case SimpleTypeKind::Float80:
  case SimpleTypeKind::Float64:
  case SimpleTypeKind::Float32:
  case SimpleTypeKind::Float16:
  case SimpleTypeKind::NarrowCharacter:
  case SimpleTypeKind::SignedCharacter:
  case SimpleTypeKind::SByte:
    return true;
  default:
    return false;
  }
}

static size_t GetTypeSizeForSimpleKind(SimpleTypeKind kind) {
  switch (kind) {
  case SimpleTypeKind::Boolean128:
  case SimpleTypeKind::Int128:
  case SimpleTypeKind::UInt128:
  case SimpleTypeKind::Float128:
    return 16;
  case SimpleTypeKind::Complex80:
  case SimpleTypeKind::Float80:
    return 10;
  case SimpleTypeKind::Boolean64:
  case SimpleTypeKind::Complex64:
  case SimpleTypeKind::UInt64:
  case SimpleTypeKind::UInt64Quad:
  case SimpleTypeKind::Float64:
  case SimpleTypeKind::Int64:
  case SimpleTypeKind::Int64Quad:
    return 8;
  case SimpleTypeKind::Boolean32:
  case SimpleTypeKind::Character32:
  case SimpleTypeKind::Complex32:
  case SimpleTypeKind::Float32:
  case SimpleTypeKind::Int32:
  case SimpleTypeKind::Int32Long:
  case SimpleTypeKind::UInt32Long:
  case SimpleTypeKind::HResult:
  case SimpleTypeKind::UInt32:
    return 4;
  case SimpleTypeKind::Boolean16:
  case SimpleTypeKind::Character16:
  case SimpleTypeKind::Float16:
  case SimpleTypeKind::Int16:
  case SimpleTypeKind::Int16Short:
  case SimpleTypeKind::UInt16:
  case SimpleTypeKind::UInt16Short:
  case SimpleTypeKind::WideCharacter:
    return 2;
  case SimpleTypeKind::Boolean8:
  case SimpleTypeKind::Byte:
  case SimpleTypeKind::UnsignedCharacter:
  case SimpleTypeKind::NarrowCharacter:
  case SimpleTypeKind::SignedCharacter:
  case SimpleTypeKind::SByte:
    return 1;
  case SimpleTypeKind::Void:
  default:
    return 0;
  }
}

std::pair<size_t, bool> GetIntegralTypeInfo(TypeIndex ti, TpiStream &tpi) {
  if (ti.isSimple()) {
    SimpleTypeKind stk = ti.getSimpleKind();
    return {GetTypeSizeForSimpleKind(stk), IsSimpleTypeSignedInteger(stk)};
  }

  CVType cvt = tpi.getType(ti);
  switch (cvt.kind()) {
  case LF_MODIFIER: {
    ModifierRecord mfr;
    llvm::cantFail(TypeDeserializer::deserializeAs<ModifierRecord>(cvt, mfr));
    return GetIntegralTypeInfo(mfr.ModifiedType, tpi);
  }
  case LF_POINTER: {
    PointerRecord pr;
    llvm::cantFail(TypeDeserializer::deserializeAs<PointerRecord>(cvt, pr));
    return GetIntegralTypeInfo(pr.ReferentType, tpi);
  }
  case LF_ENUM: {
    EnumRecord er;
    llvm::cantFail(TypeDeserializer::deserializeAs<EnumRecord>(cvt, er));
    return GetIntegralTypeInfo(er.UnderlyingType, tpi);
  }
  default:
    assert(false && "Type is not integral!");
    return {0, false};
  }
}

static llvm::StringRef GetSimpleTypeName(SimpleTypeKind kind) {
  switch (kind) {
  case SimpleTypeKind::Boolean128:
  case SimpleTypeKind::Boolean16:
  case SimpleTypeKind::Boolean32:
  case SimpleTypeKind::Boolean64:
  case SimpleTypeKind::Boolean8:
    return "bool";
  case SimpleTypeKind::Byte:
  case SimpleTypeKind::UnsignedCharacter:
    return "unsigned char";
  case SimpleTypeKind::NarrowCharacter:
    return "char";
  case SimpleTypeKind::SignedCharacter:
  case SimpleTypeKind::SByte:
    return "signed char";
  case SimpleTypeKind::Character16:
    return "char16_t";
  case SimpleTypeKind::Character32:
    return "char32_t";
  case SimpleTypeKind::Complex80:
  case SimpleTypeKind::Complex64:
  case SimpleTypeKind::Complex32:
    return "complex";
  case SimpleTypeKind::Float128:
  case SimpleTypeKind::Float80:
    return "long double";
  case SimpleTypeKind::Float64:
    return "double";
  case SimpleTypeKind::Float32:
    return "float";
  case SimpleTypeKind::Float16:
    return "single";
  case SimpleTypeKind::Int128:
    return "__int128";
  case SimpleTypeKind::Int64:
  case SimpleTypeKind::Int64Quad:
    return "int64_t";
  case SimpleTypeKind::Int32:
    return "int";
  case SimpleTypeKind::Int16:
    return "short";
  case SimpleTypeKind::UInt128:
    return "unsigned __int128";
  case SimpleTypeKind::UInt64:
  case SimpleTypeKind::UInt64Quad:
    return "uint64_t";
  case SimpleTypeKind::HResult:
    return "HRESULT";
  case SimpleTypeKind::UInt32:
    return "unsigned";
  case SimpleTypeKind::UInt16:
  case SimpleTypeKind::UInt16Short:
    return "unsigned short";
  case SimpleTypeKind::Int32Long:
    return "long";
  case SimpleTypeKind::UInt32Long:
    return "unsigned long";
  case SimpleTypeKind::Void:
    return "void";
  case SimpleTypeKind::WideCharacter:
    return "wchar_t";
  default:
    return "";
  }
}

static bool IsClassRecord(TypeLeafKind kind) {
  switch (kind) {
  case LF_STRUCTURE:
  case LF_CLASS:
  case LF_INTERFACE:
    return true;
  default:
    return false;
  }
}

static PDB_SymType GetPdbSymType(TpiStream &tpi, TypeIndex ti) {
  if (ti.isSimple()) {
    if (ti.getSimpleMode() == SimpleTypeMode::Direct)
      return PDB_SymType::BuiltinType;
    return PDB_SymType::PointerType;
  }

  CVType cvt = tpi.getType(ti);
  TypeLeafKind kind = cvt.kind();
  if (kind != LF_MODIFIER)
    return CVTypeToPDBType(kind);

  // If this is an LF_MODIFIER, look through it to get the kind that it
  // modifies.  Note that it's not possible to have an LF_MODIFIER that
  // modifies another LF_MODIFIER, although this would handle that anyway.
  return GetPdbSymType(tpi, LookThroughModifierRecord(cvt));
}

static bool IsCVarArgsFunction(llvm::ArrayRef<TypeIndex> args) {
  if (args.empty())
    return false;
  return args.back() == TypeIndex::None();
}

static clang::TagTypeKind TranslateUdtKind(const TagRecord &cr) {
  switch (cr.Kind) {
  case TypeRecordKind::Class:
    return clang::TTK_Class;
  case TypeRecordKind::Struct:
    return clang::TTK_Struct;
  case TypeRecordKind::Union:
    return clang::TTK_Union;
  case TypeRecordKind::Interface:
    return clang::TTK_Interface;
  case TypeRecordKind::Enum:
    return clang::TTK_Enum;
  default:
    lldbassert(false && "Invalid tag record kind!");
    return clang::TTK_Struct;
  }
}

static llvm::Optional<clang::CallingConv>
TranslateCallingConvention(llvm::codeview::CallingConvention conv) {
  using CC = llvm::codeview::CallingConvention;
  switch (conv) {

  case CC::NearC:
  case CC::FarC:
    return clang::CallingConv::CC_C;
  case CC::NearPascal:
  case CC::FarPascal:
    return clang::CallingConv::CC_X86Pascal;
  case CC::NearFast:
  case CC::FarFast:
    return clang::CallingConv::CC_X86FastCall;
  case CC::NearStdCall:
  case CC::FarStdCall:
    return clang::CallingConv::CC_X86StdCall;
  case CC::ThisCall:
    return clang::CallingConv::CC_X86ThisCall;
  case CC::NearVector:
    return clang::CallingConv::CC_X86VectorCall;
  default:
    return llvm::None;
  }
}

void SymbolFileNativePDB::Initialize() {
  PluginManager::RegisterPlugin(GetPluginNameStatic(),
                                GetPluginDescriptionStatic(), CreateInstance,
                                DebuggerInitialize);
}

void SymbolFileNativePDB::Terminate() {
  PluginManager::UnregisterPlugin(CreateInstance);
}

void SymbolFileNativePDB::DebuggerInitialize(Debugger &debugger) {}

ConstString SymbolFileNativePDB::GetPluginNameStatic() {
  static ConstString g_name("native-pdb");
  return g_name;
}

const char *SymbolFileNativePDB::GetPluginDescriptionStatic() {
  return "Microsoft PDB debug symbol cross-platform file reader.";
}

SymbolFile *SymbolFileNativePDB::CreateInstance(ObjectFile *obj_file) {
  return new SymbolFileNativePDB(obj_file);
}

SymbolFileNativePDB::SymbolFileNativePDB(ObjectFile *object_file)
    : SymbolFile(object_file) {}

SymbolFileNativePDB::~SymbolFileNativePDB() {}

uint32_t SymbolFileNativePDB::CalculateAbilities() {
  uint32_t abilities = 0;
  if (!m_obj_file)
    return 0;

  if (!m_index) {
    // Lazily load and match the PDB file, but only do this once.
    std::unique_ptr<PDBFile> file_up =
        loadMatchingPDBFile(m_obj_file->GetFileSpec().GetPath(), m_allocator);

    if (!file_up) {
      auto module_sp = m_obj_file->GetModule();
      if (!module_sp)
        return 0;
      // See if any symbol file is specified through `--symfile` option.
      FileSpec symfile = module_sp->GetSymbolFileFileSpec();
      if (!symfile)
        return 0;
      file_up = loadPDBFile(symfile.GetPath(), m_allocator);
    }

    if (!file_up)
      return 0;

    auto expected_index = PdbIndex::create(std::move(file_up));
    if (!expected_index) {
      llvm::consumeError(expected_index.takeError());
      return 0;
    }
    m_index = std::move(*expected_index);
  }
  if (!m_index)
    return 0;

  // We don't especially have to be precise here.  We only distinguish between
  // stripped and not stripped.
  abilities = kAllAbilities;

  if (m_index->dbi().isStripped())
    abilities &= ~(Blocks | LocalVariables);
  return abilities;
}

void SymbolFileNativePDB::InitializeObject() {
  m_obj_load_address = m_obj_file->GetFileOffset();
  m_index->SetLoadAddress(m_obj_load_address);
  m_index->ParseSectionContribs();

  TypeSystem *ts = GetTypeSystemForLanguage(eLanguageTypeC_plus_plus);
  m_clang = llvm::dyn_cast_or_null<ClangASTContext>(ts);
  m_importer = llvm::make_unique<ClangASTImporter>();

  PreprocessTpiStream();
  lldbassert(m_clang);
}

static llvm::Optional<CVTagRecord>
GetNestedTagRecord(const NestedTypeRecord &Record, const CVTagRecord &parent,
                   TpiStream &tpi) {
  // An LF_NESTTYPE is essentially a nested typedef / using declaration, but it
  // is also used to indicate the primary definition of a nested class.  That is
  // to say, if you have:
  // struct A {
  //   struct B {};
  //   using C = B;
  // };
  // Then in the debug info, this will appear as:
  // LF_STRUCTURE `A::B` [type index = N]
  // LF_STRUCTURE `A`
  //   LF_NESTTYPE [name = `B`, index = N]
  //   LF_NESTTYPE [name = `C`, index = N]
  // In order to accurately reconstruct the decl context hierarchy, we need to
  // know which ones are actual definitions and which ones are just aliases.

  // If it's a simple type, then this is something like `using foo = int`.
  if (Record.Type.isSimple())
    return llvm::None;

  CVType cvt = tpi.getType(Record.Type);

  if (!IsTagRecord(cvt))
    return llvm::None;

  // If it's an inner definition, then treat whatever name we have here as a
  // single component of a mangled name.  So we can inject it into the parent's
  // mangled name to see if it matches.
  CVTagRecord child = CVTagRecord::create(cvt);
  std::string qname = parent.asTag().getUniqueName();
  if (qname.size() < 4 || child.asTag().getUniqueName().size() < 4)
    return llvm::None;

  // qname[3] is the tag type identifier (struct, class, union, etc).  Since the
  // inner tag type is not necessarily the same as the outer tag type, re-write
  // it to match the inner tag type.
  qname[3] = child.asTag().getUniqueName()[3];
  std::string piece = Record.Name;
  piece.push_back('@');
  qname.insert(4, std::move(piece));
  if (qname != child.asTag().UniqueName)
    return llvm::None;

  return std::move(child);
}

void SymbolFileNativePDB::PreprocessTpiStream() {
  LazyRandomTypeCollection &types = m_index->tpi().typeCollection();

  for (auto ti = types.getFirst(); ti; ti = types.getNext(*ti)) {
    CVType type = types.getType(*ti);
    if (!IsTagRecord(type))
      continue;

    CVTagRecord tag = CVTagRecord::create(type);
    // We're looking for LF_NESTTYPE records in the field list, so ignore
    // forward references (no field list), and anything without a nested class
    // (since there won't be any LF_NESTTYPE records).
    if (tag.asTag().isForwardRef() || !tag.asTag().containsNestedClass())
      continue;

    struct ProcessTpiStream : public TypeVisitorCallbacks {
      ProcessTpiStream(PdbIndex &index, TypeIndex parent,
                       const CVTagRecord &parent_cvt,
                       llvm::DenseMap<TypeIndex, TypeIndex> &parents)
          : index(index), parents(parents), parent(parent),
            parent_cvt(parent_cvt) {}

      PdbIndex &index;
      llvm::DenseMap<TypeIndex, TypeIndex> &parents;
      TypeIndex parent;
      const CVTagRecord &parent_cvt;

      llvm::Error visitKnownMember(CVMemberRecord &CVR,
                                   NestedTypeRecord &Record) override {
        llvm::Optional<CVTagRecord> tag =
            GetNestedTagRecord(Record, parent_cvt, index.tpi());
        if (!tag)
          return llvm::ErrorSuccess();

        parents[Record.Type] = parent;
        if (!tag->asTag().isForwardRef())
          return llvm::ErrorSuccess();

        llvm::Expected<TypeIndex> full_decl =
            index.tpi().findFullDeclForForwardRef(Record.Type);
        if (!full_decl) {
          llvm::consumeError(full_decl.takeError());
          return llvm::ErrorSuccess();
        }
        parents[*full_decl] = parent;
        return llvm::ErrorSuccess();
      }
    };

    CVType field_list = m_index->tpi().getType(tag.asTag().FieldList);
    ProcessTpiStream process(*m_index, *ti, tag, m_parent_types);
    llvm::Error error = visitMemberRecordStream(field_list.data(), process);
    if (error)
      llvm::consumeError(std::move(error));
  }
}

uint32_t SymbolFileNativePDB::GetNumCompileUnits() {
  const DbiModuleList &modules = m_index->dbi().modules();
  uint32_t count = modules.getModuleCount();
  if (count == 0)
    return count;

  // The linker can inject an additional "dummy" compilation unit into the
  // PDB. Ignore this special compile unit for our purposes, if it is there.
  // It is always the last one.
  DbiModuleDescriptor last = modules.getModuleDescriptor(count - 1);
  if (last.getModuleName() == "* Linker *")
    --count;
  return count;
}

lldb::FunctionSP SymbolFileNativePDB::CreateFunction(PdbCompilandSymId func_id,
                                                     const SymbolContext &sc) {
  const CompilandIndexItem *cci =
      m_index->compilands().GetCompiland(func_id.modi);
  lldbassert(cci);
  CVSymbol sym_record = cci->m_debug_stream.readSymbolAtOffset(func_id.offset);

  lldbassert(sym_record.kind() == S_LPROC32 || sym_record.kind() == S_GPROC32);
  SegmentOffsetLength sol = GetSegmentOffsetAndLength(sym_record);

  auto file_vm_addr = m_index->MakeVirtualAddress(sol.so);
  if (file_vm_addr == LLDB_INVALID_ADDRESS || file_vm_addr == 0)
    return nullptr;

  AddressRange func_range(file_vm_addr, sol.length,
                          sc.module_sp->GetSectionList());
  if (!func_range.GetBaseAddress().IsValid())
    return nullptr;

  Type *func_type = nullptr;

  // FIXME: Resolve types and mangled names.
  PdbTypeSymId sig_id{TypeIndex::None(), false};
  Mangled mangled(getSymbolName(sym_record));
  FunctionSP func_sp = std::make_shared<Function>(
      sc.comp_unit, toOpaqueUid(func_id), toOpaqueUid(sig_id), mangled,
      func_type, func_range);

  sc.comp_unit->AddFunction(func_sp);
  return func_sp;
}

CompUnitSP
SymbolFileNativePDB::CreateCompileUnit(const CompilandIndexItem &cci) {
  lldb::LanguageType lang =
      cci.m_compile_opts ? TranslateLanguage(cci.m_compile_opts->getLanguage())
                         : lldb::eLanguageTypeUnknown;

  LazyBool optimized = eLazyBoolNo;
  if (cci.m_compile_opts && cci.m_compile_opts->hasOptimizations())
    optimized = eLazyBoolYes;

  llvm::StringRef source_file_name =
      m_index->compilands().GetMainSourceFile(cci);
  FileSpec fs(source_file_name);

  CompUnitSP cu_sp =
      std::make_shared<CompileUnit>(m_obj_file->GetModule(), nullptr, fs,
                                    toOpaqueUid(cci.m_id), lang, optimized);

  m_obj_file->GetModule()->GetSymbolVendor()->SetCompileUnitAtIndex(
      cci.m_id.modi, cu_sp);
  return cu_sp;
}

lldb::TypeSP SymbolFileNativePDB::CreateModifierType(PdbTypeSymId type_id,
                                                     const ModifierRecord &mr) {
  TpiStream &stream = m_index->tpi();

  TypeSP t = GetOrCreateType(mr.ModifiedType);
  CompilerType ct = t->GetForwardCompilerType();
  if ((mr.Modifiers & ModifierOptions::Const) != ModifierOptions::None)
    ct = ct.AddConstModifier();
  if ((mr.Modifiers & ModifierOptions::Volatile) != ModifierOptions::None)
    ct = ct.AddVolatileModifier();
  std::string name;
  if (mr.ModifiedType.isSimple())
    name = GetSimpleTypeName(mr.ModifiedType.getSimpleKind());
  else
    name = computeTypeName(stream.typeCollection(), mr.ModifiedType);
  Declaration decl;
  return std::make_shared<Type>(toOpaqueUid(type_id), m_clang->GetSymbolFile(),
                                ConstString(name), t->GetByteSize(), nullptr,
                                LLDB_INVALID_UID, Type::eEncodingIsUID, decl,
                                ct, Type::eResolveStateFull);
}

lldb::TypeSP SymbolFileNativePDB::CreatePointerType(
    PdbTypeSymId type_id, const llvm::codeview::PointerRecord &pr) {
  TypeSP pointee = GetOrCreateType(pr.ReferentType);
  if (!pointee)
    return nullptr;
  CompilerType pointee_ct = pointee->GetForwardCompilerType();
  lldbassert(pointee_ct);
  Declaration decl;

  if (pr.isPointerToMember()) {
    MemberPointerInfo mpi = pr.getMemberInfo();
    TypeSP class_type = GetOrCreateType(mpi.ContainingType);

    CompilerType ct = ClangASTContext::CreateMemberPointerType(
        class_type->GetLayoutCompilerType(), pointee_ct);

    return std::make_shared<Type>(
        toOpaqueUid(type_id), m_clang->GetSymbolFile(), ConstString(),
        pr.getSize(), nullptr, LLDB_INVALID_UID, Type::eEncodingIsUID, decl, ct,
        Type::eResolveStateFull);
  }

  CompilerType pointer_ct = pointee_ct;
  if (pr.getMode() == PointerMode::LValueReference)
    pointer_ct = pointer_ct.GetLValueReferenceType();
  else if (pr.getMode() == PointerMode::RValueReference)
    pointer_ct = pointer_ct.GetRValueReferenceType();
  else
    pointer_ct = pointer_ct.GetPointerType();

  if ((pr.getOptions() & PointerOptions::Const) != PointerOptions::None)
    pointer_ct = pointer_ct.AddConstModifier();

  if ((pr.getOptions() & PointerOptions::Volatile) != PointerOptions::None)
    pointer_ct = pointer_ct.AddVolatileModifier();

  if ((pr.getOptions() & PointerOptions::Restrict) != PointerOptions::None)
    pointer_ct = pointer_ct.AddRestrictModifier();

  return std::make_shared<Type>(toOpaqueUid(type_id), m_clang->GetSymbolFile(),
                                ConstString(), pr.getSize(), nullptr,
                                LLDB_INVALID_UID, Type::eEncodingIsUID, decl,
                                pointer_ct, Type::eResolveStateFull);
}

lldb::TypeSP SymbolFileNativePDB::CreateSimpleType(TypeIndex ti) {
  uint64_t uid = toOpaqueUid(PdbTypeSymId{ti, false});
  if (ti == TypeIndex::NullptrT()) {
    CompilerType ct = m_clang->GetBasicType(eBasicTypeNullPtr);
    Declaration decl;
    return std::make_shared<Type>(
        uid, this, ConstString("std::nullptr_t"), 0, nullptr, LLDB_INVALID_UID,
        Type::eEncodingIsUID, decl, ct, Type::eResolveStateFull);
  }

  if (ti.getSimpleMode() != SimpleTypeMode::Direct) {
    TypeSP direct_sp = GetOrCreateType(ti.makeDirect());
    CompilerType ct = direct_sp->GetFullCompilerType();
    ct = ct.GetPointerType();
    uint32_t pointer_size = 0;
    switch (ti.getSimpleMode()) {
    case SimpleTypeMode::FarPointer32:
    case SimpleTypeMode::NearPointer32:
      pointer_size = 4;
      break;
    case SimpleTypeMode::NearPointer64:
      pointer_size = 8;
      break;
    default:
      // 128-bit and 16-bit pointers unsupported.
      return nullptr;
    }
    Declaration decl;
    return std::make_shared<Type>(uid, m_clang->GetSymbolFile(), ConstString(),
                                  pointer_size, nullptr, LLDB_INVALID_UID,
                                  Type::eEncodingIsUID, decl, ct,
                                  Type::eResolveStateFull);
  }

  if (ti.getSimpleKind() == SimpleTypeKind::NotTranslated)
    return nullptr;

  lldb::BasicType bt = GetCompilerTypeForSimpleKind(ti.getSimpleKind());
  if (bt == lldb::eBasicTypeInvalid)
    return nullptr;
  CompilerType ct = m_clang->GetBasicType(bt);
  size_t size = GetTypeSizeForSimpleKind(ti.getSimpleKind());

  llvm::StringRef type_name = GetSimpleTypeName(ti.getSimpleKind());

  Declaration decl;
  return std::make_shared<Type>(uid, m_clang->GetSymbolFile(),
                                ConstString(type_name), size, nullptr,
                                LLDB_INVALID_UID, Type::eEncodingIsUID, decl,
                                ct, Type::eResolveStateFull);
}

static std::string RenderDemanglerNode(llvm::ms_demangle::Node *n) {
  OutputStream OS;
  initializeOutputStream(nullptr, nullptr, OS, 1024);
  n->output(OS, llvm::ms_demangle::OF_Default);
  OS << '\0';
  return {OS.getBuffer()};
}

static bool
AnyScopesHaveTemplateParams(llvm::ArrayRef<llvm::ms_demangle::Node *> scopes) {
  for (llvm::ms_demangle::Node *n : scopes) {
    auto *idn = static_cast<llvm::ms_demangle::IdentifierNode *>(n);
    if (idn->TemplateParams)
      return true;
  }
  return false;
}

std::pair<clang::DeclContext *, std::string>
SymbolFileNativePDB::CreateDeclInfoForType(const TagRecord &record,
                                           TypeIndex ti) {
  llvm::ms_demangle::Demangler demangler;
  StringView sv(record.UniqueName.begin(), record.UniqueName.size());
  llvm::ms_demangle::TagTypeNode *ttn = demangler.parseTagUniqueName(sv);
  llvm::ms_demangle::IdentifierNode *idn =
      ttn->QualifiedName->getUnqualifiedIdentifier();
  std::string uname = RenderDemanglerNode(idn);

  llvm::ms_demangle::NodeArrayNode *name_components =
      ttn->QualifiedName->Components;
  llvm::ArrayRef<llvm::ms_demangle::Node *> scopes(name_components->Nodes,
                                                   name_components->Count - 1);

  clang::DeclContext *context = m_clang->GetTranslationUnitDecl();

  // If this type doesn't have a parent type in the debug info, then the best we
  // can do is to say that it's either a series of namespaces (if the scope is
  // non-empty), or the translation unit (if the scope is empty).
  auto parent_iter = m_parent_types.find(ti);
  if (parent_iter == m_parent_types.end()) {
    if (scopes.empty())
      return {context, uname};

    // If there is no parent in the debug info, but some of the scopes have
    // template params, then this is a case of bad debug info.  See, for
    // example, llvm.org/pr39607.  We don't want to create an ambiguity between
    // a NamespaceDecl and a CXXRecordDecl, so instead we create a class at
    // global scope with the fully qualified name.
    if (AnyScopesHaveTemplateParams(scopes))
      return {context, record.Name};

    for (llvm::ms_demangle::Node *scope : scopes) {
      auto *nii = static_cast<llvm::ms_demangle::NamedIdentifierNode *>(scope);
      std::string str = RenderDemanglerNode(nii);
      context = m_clang->GetUniqueNamespaceDeclaration(str.c_str(), context);
    }
    return {context, uname};
  }

  // Otherwise, all we need to do is get the parent type of this type and
  // recurse into our lazy type creation / AST reconstruction logic to get an
  // LLDB TypeSP for the parent.  This will cause the AST to automatically get
  // the right DeclContext created for any parent.
  TypeSP parent = GetOrCreateType(parent_iter->second);
  if (!parent)
    return {context, uname};
  CompilerType parent_ct = parent->GetForwardCompilerType();
  clang::QualType qt = ClangUtil::GetCanonicalQualType(parent_ct);
  context = clang::TagDecl::castToDeclContext(qt->getAsTagDecl());
  return {context, uname};
}

lldb::TypeSP SymbolFileNativePDB::CreateClassStructUnion(
    PdbTypeSymId type_id, const llvm::codeview::TagRecord &record, size_t size,
    clang::TagTypeKind ttk, clang::MSInheritanceAttr::Spelling inheritance) {

  clang::DeclContext *decl_context = nullptr;
  std::string uname;
  std::tie(decl_context, uname) = CreateDeclInfoForType(record, type_id.index);

  lldb::AccessType access =
      (ttk == clang::TTK_Class) ? lldb::eAccessPrivate : lldb::eAccessPublic;

  ClangASTMetadata metadata;
  metadata.SetUserID(toOpaqueUid(type_id));
  metadata.SetIsDynamicCXXType(false);

  CompilerType ct =
      m_clang->CreateRecordType(decl_context, access, uname.c_str(), ttk,
                                lldb::eLanguageTypeC_plus_plus, &metadata);

  lldbassert(ct.IsValid());

  clang::CXXRecordDecl *record_decl =
      m_clang->GetAsCXXRecordDecl(ct.GetOpaqueQualType());
  lldbassert(record_decl);

  clang::MSInheritanceAttr *attr = clang::MSInheritanceAttr::CreateImplicit(
      *m_clang->getASTContext(), inheritance);
  record_decl->addAttr(attr);

  ClangASTContext::StartTagDeclarationDefinition(ct);

  // Even if it's possible, don't complete it at this point. Just mark it
  // forward resolved, and if/when LLDB needs the full definition, it can
  // ask us.
  ClangASTContext::SetHasExternalStorage(ct.GetOpaqueQualType(), true);

  // FIXME: Search IPI stream for LF_UDT_MOD_SRC_LINE.
  Declaration decl;
  return std::make_shared<Type>(toOpaqueUid(type_id), m_clang->GetSymbolFile(),
                                ConstString(uname), size, nullptr,
                                LLDB_INVALID_UID, Type::eEncodingIsUID, decl,
                                ct, Type::eResolveStateForward);
}

lldb::TypeSP SymbolFileNativePDB::CreateTagType(PdbTypeSymId type_id,
                                                const ClassRecord &cr) {
  clang::TagTypeKind ttk = TranslateUdtKind(cr);

  clang::MSInheritanceAttr::Spelling inheritance =
      GetMSInheritance(m_index->tpi().typeCollection(), cr);
  return CreateClassStructUnion(type_id, cr, cr.getSize(), ttk, inheritance);
}

lldb::TypeSP SymbolFileNativePDB::CreateTagType(PdbTypeSymId type_id,
                                                const UnionRecord &ur) {
  return CreateClassStructUnion(
      type_id, ur, ur.getSize(), clang::TTK_Union,
      clang::MSInheritanceAttr::Spelling::Keyword_single_inheritance);
}

lldb::TypeSP SymbolFileNativePDB::CreateTagType(PdbTypeSymId type_id,
                                                const EnumRecord &er) {
  clang::DeclContext *decl_context = nullptr;
  std::string uname;
  std::tie(decl_context, uname) = CreateDeclInfoForType(er, type_id.index);

  Declaration decl;
  TypeSP underlying_type = GetOrCreateType(er.UnderlyingType);
  CompilerType enum_ct = m_clang->CreateEnumerationType(
      uname.c_str(), decl_context, decl, underlying_type->GetFullCompilerType(),
      er.isScoped());

  ClangASTContext::StartTagDeclarationDefinition(enum_ct);
  ClangASTContext::SetHasExternalStorage(enum_ct.GetOpaqueQualType(), true);

  // We're just going to forward resolve this for now.  We'll complete
  // it only if the user requests.
  return std::make_shared<lldb_private::Type>(
      toOpaqueUid(type_id), m_clang->GetSymbolFile(), ConstString(uname),
      underlying_type->GetByteSize(), nullptr, LLDB_INVALID_UID,
      lldb_private::Type::eEncodingIsUID, decl, enum_ct,
      lldb_private::Type::eResolveStateForward);
}

TypeSP SymbolFileNativePDB::CreateArrayType(PdbTypeSymId type_id,
                                            const ArrayRecord &ar) {
  TypeSP element_type = GetOrCreateType(ar.ElementType);
  uint64_t element_count = ar.Size / element_type->GetByteSize();

  CompilerType element_ct = element_type->GetFullCompilerType();

  CompilerType array_ct =
      m_clang->CreateArrayType(element_ct, element_count, false);

  Declaration decl;
  TypeSP array_sp = std::make_shared<lldb_private::Type>(
      toOpaqueUid(type_id), m_clang->GetSymbolFile(), ConstString(), ar.Size,
      nullptr, LLDB_INVALID_UID, lldb_private::Type::eEncodingIsUID, decl,
      array_ct, lldb_private::Type::eResolveStateFull);
  array_sp->SetEncodingType(element_type.get());
  return array_sp;
}

TypeSP SymbolFileNativePDB::CreateProcedureType(PdbTypeSymId type_id,
                                                const ProcedureRecord &pr) {
  TpiStream &stream = m_index->tpi();
  CVType args_cvt = stream.getType(pr.ArgumentList);
  ArgListRecord args;
  llvm::cantFail(
      TypeDeserializer::deserializeAs<ArgListRecord>(args_cvt, args));

  llvm::ArrayRef<TypeIndex> arg_indices = llvm::makeArrayRef(args.ArgIndices);
  bool is_variadic = IsCVarArgsFunction(arg_indices);
  if (is_variadic)
    arg_indices = arg_indices.drop_back();

  std::vector<CompilerType> arg_list;
  arg_list.reserve(arg_list.size());

  for (TypeIndex arg_index : arg_indices) {
    TypeSP arg_sp = GetOrCreateType(arg_index);
    if (!arg_sp)
      return nullptr;
    arg_list.push_back(arg_sp->GetFullCompilerType());
  }

  TypeSP return_type_sp = GetOrCreateType(pr.ReturnType);
  if (!return_type_sp)
    return nullptr;

  llvm::Optional<clang::CallingConv> cc =
      TranslateCallingConvention(pr.CallConv);
  if (!cc)
    return nullptr;

  CompilerType return_ct = return_type_sp->GetFullCompilerType();
  CompilerType func_sig_ast_type = m_clang->CreateFunctionType(
      return_ct, arg_list.data(), arg_list.size(), is_variadic, 0, *cc);

  Declaration decl;
  return std::make_shared<lldb_private::Type>(
      toOpaqueUid(type_id), this, ConstString(), 0, nullptr, LLDB_INVALID_UID,
      lldb_private::Type::eEncodingIsUID, decl, func_sig_ast_type,
      lldb_private::Type::eResolveStateFull);
}

TypeSP SymbolFileNativePDB::CreateType(PdbTypeSymId type_id) {
  if (type_id.index.isSimple())
    return CreateSimpleType(type_id.index);

  TpiStream &stream = type_id.is_ipi ? m_index->ipi() : m_index->tpi();
  CVType cvt = stream.getType(type_id.index);

  if (cvt.kind() == LF_MODIFIER) {
    ModifierRecord modifier;
    llvm::cantFail(
        TypeDeserializer::deserializeAs<ModifierRecord>(cvt, modifier));
    return CreateModifierType(type_id, modifier);
  }

  if (cvt.kind() == LF_POINTER) {
    PointerRecord pointer;
    llvm::cantFail(
        TypeDeserializer::deserializeAs<PointerRecord>(cvt, pointer));
    return CreatePointerType(type_id, pointer);
  }

  if (IsClassRecord(cvt.kind())) {
    ClassRecord cr;
    llvm::cantFail(TypeDeserializer::deserializeAs<ClassRecord>(cvt, cr));
    return CreateTagType(type_id, cr);
  }

  if (cvt.kind() == LF_ENUM) {
    EnumRecord er;
    llvm::cantFail(TypeDeserializer::deserializeAs<EnumRecord>(cvt, er));
    return CreateTagType(type_id, er);
  }

  if (cvt.kind() == LF_UNION) {
    UnionRecord ur;
    llvm::cantFail(TypeDeserializer::deserializeAs<UnionRecord>(cvt, ur));
    return CreateTagType(type_id, ur);
  }

  if (cvt.kind() == LF_ARRAY) {
    ArrayRecord ar;
    llvm::cantFail(TypeDeserializer::deserializeAs<ArrayRecord>(cvt, ar));
    return CreateArrayType(type_id, ar);
  }

  if (cvt.kind() == LF_PROCEDURE) {
    ProcedureRecord pr;
    llvm::cantFail(TypeDeserializer::deserializeAs<ProcedureRecord>(cvt, pr));
    return CreateProcedureType(type_id, pr);
  }

  return nullptr;
}

TypeSP SymbolFileNativePDB::CreateAndCacheType(PdbTypeSymId type_id) {
  // If they search for a UDT which is a forward ref, try and resolve the full
  // decl and just map the forward ref uid to the full decl record.
  llvm::Optional<PdbTypeSymId> full_decl_uid;
  if (IsForwardRefUdt(type_id, m_index->tpi())) {
    auto expected_full_ti =
        m_index->tpi().findFullDeclForForwardRef(type_id.index);
    if (!expected_full_ti)
      llvm::consumeError(expected_full_ti.takeError());
    else if (*expected_full_ti != type_id.index) {
      full_decl_uid = PdbTypeSymId{*expected_full_ti, false};

      // It's possible that a lookup would occur for the full decl causing it
      // to be cached, then a second lookup would occur for the forward decl.
      // We don't want to create a second full decl, so make sure the full
      // decl hasn't already been cached.
      auto full_iter = m_types.find(toOpaqueUid(*full_decl_uid));
      if (full_iter != m_types.end()) {
        TypeSP result = full_iter->second;
        // Map the forward decl to the TypeSP for the full decl so we can take
        // the fast path next time.
        m_types[toOpaqueUid(type_id)] = result;
        return result;
      }
    }
  }

  PdbTypeSymId best_decl_id = full_decl_uid ? *full_decl_uid : type_id;
  TypeSP result = CreateType(best_decl_id);
  if (!result)
    return nullptr;

  uint64_t best_uid = toOpaqueUid(best_decl_id);
  m_types[best_uid] = result;
  // If we had both a forward decl and a full decl, make both point to the new
  // type.
  if (full_decl_uid)
    m_types[toOpaqueUid(type_id)] = result;

  if (IsTagRecord(best_decl_id, m_index->tpi())) {
    clang::TagDecl *record_decl =
        m_clang->GetAsTagDecl(result->GetForwardCompilerType());
    lldbassert(record_decl);

    TypeIndex ti(type_id.index);
    m_uid_to_decl[best_uid] = record_decl;
    m_decl_to_status[record_decl] =
        DeclStatus(best_uid, Type::eResolveStateForward);
  }
  return result;
}

TypeSP SymbolFileNativePDB::GetOrCreateType(PdbTypeSymId type_id) {
  // We can't use try_emplace / overwrite here because the process of creating
  // a type could create nested types, which could invalidate iterators.  So
  // we have to do a 2-phase lookup / insert.
  auto iter = m_types.find(toOpaqueUid(type_id));
  if (iter != m_types.end())
    return iter->second;

  return CreateAndCacheType(type_id);
}

static DWARFExpression
MakeConstantLocationExpression(TypeIndex underlying_ti, TpiStream &tpi,
                               const ConstantSym &constant, ModuleSP module) {
  const ArchSpec &architecture = module->GetArchitecture();
  uint32_t address_size = architecture.GetAddressByteSize();

  size_t size = 0;
  bool is_signed = false;
  std::tie(size, is_signed) = GetIntegralTypeInfo(underlying_ti, tpi);

  union {
    llvm::support::little64_t I;
    llvm::support::ulittle64_t U;
  } Value;

  std::shared_ptr<DataBufferHeap> buffer = std::make_shared<DataBufferHeap>();
  buffer->SetByteSize(size);

  llvm::ArrayRef<uint8_t> bytes;
  if (is_signed) {
    Value.I = constant.Value.getSExtValue();
  } else {
    Value.U = constant.Value.getZExtValue();
  }

  bytes = llvm::makeArrayRef(reinterpret_cast<const uint8_t *>(&Value), 8)
              .take_front(size);
  buffer->CopyData(bytes.data(), size);
  DataExtractor extractor(buffer, lldb::eByteOrderLittle, address_size);
  DWARFExpression result(nullptr, extractor, nullptr, 0, size);
  return result;
}

static DWARFExpression MakeGlobalLocationExpression(uint16_t section,
                                                    uint32_t offset,
                                                    ModuleSP module) {
  assert(section > 0);
  assert(module);

  const ArchSpec &architecture = module->GetArchitecture();
  ByteOrder byte_order = architecture.GetByteOrder();
  uint32_t address_size = architecture.GetAddressByteSize();
  uint32_t byte_size = architecture.GetDataByteSize();
  assert(byte_order != eByteOrderInvalid && address_size != 0);

  RegisterKind register_kind = eRegisterKindDWARF;
  StreamBuffer<32> stream(Stream::eBinary, address_size, byte_order);
  stream.PutHex8(DW_OP_addr);

  SectionList *section_list = module->GetSectionList();
  assert(section_list);

  // Section indices in PDB are 1-based, but in DWARF they are 0-based, so we
  // need to subtract 1.
  uint32_t section_idx = section - 1;
  if (section_idx >= section_list->GetSize())
    return DWARFExpression(nullptr);

  auto section_ptr = section_list->GetSectionAtIndex(section_idx);
  if (!section_ptr)
    return DWARFExpression(nullptr);

  stream.PutMaxHex64(section_ptr->GetFileAddress() + offset, address_size,
                     byte_order);
  DataBufferSP buffer =
      std::make_shared<DataBufferHeap>(stream.GetData(), stream.GetSize());
  DataExtractor extractor(buffer, byte_order, address_size, byte_size);
  DWARFExpression result(module, extractor, nullptr, 0, buffer->GetByteSize());
  result.SetRegisterKind(register_kind);
  return result;
}

VariableSP SymbolFileNativePDB::CreateGlobalVariable(PdbGlobalSymId var_id) {
  CVSymbol sym = m_index->symrecords().readRecord(var_id.offset);
  if (sym.kind() == S_CONSTANT)
    return CreateConstantSymbol(var_id, sym);

  lldb::ValueType scope = eValueTypeInvalid;
  TypeIndex ti;
  llvm::StringRef name;
  lldb::addr_t addr = 0;
  uint16_t section = 0;
  uint32_t offset = 0;
  bool is_external = false;
  switch (sym.kind()) {
  case S_GDATA32:
    is_external = true;
    LLVM_FALLTHROUGH;
  case S_LDATA32: {
    DataSym ds(sym.kind());
    llvm::cantFail(SymbolDeserializer::deserializeAs<DataSym>(sym, ds));
    ti = ds.Type;
    scope = (sym.kind() == S_GDATA32) ? eValueTypeVariableGlobal
                                      : eValueTypeVariableStatic;
    name = ds.Name;
    section = ds.Segment;
    offset = ds.DataOffset;
    addr = m_index->MakeVirtualAddress(ds.Segment, ds.DataOffset);
    break;
  }
  case S_GTHREAD32:
    is_external = true;
    LLVM_FALLTHROUGH;
  case S_LTHREAD32: {
    ThreadLocalDataSym tlds(sym.kind());
    llvm::cantFail(
        SymbolDeserializer::deserializeAs<ThreadLocalDataSym>(sym, tlds));
    ti = tlds.Type;
    name = tlds.Name;
    section = tlds.Segment;
    offset = tlds.DataOffset;
    addr = m_index->MakeVirtualAddress(tlds.Segment, tlds.DataOffset);
    scope = eValueTypeVariableThreadLocal;
    break;
  }
  default:
    llvm_unreachable("unreachable!");
  }

  CompUnitSP comp_unit;
  llvm::Optional<uint16_t> modi = m_index->GetModuleIndexForVa(addr);
  if (modi) {
    CompilandIndexItem &cci = m_index->compilands().GetOrCreateCompiland(*modi);
    comp_unit = GetOrCreateCompileUnit(cci);
  }

  Declaration decl;
  PdbTypeSymId tid{ti, false};
  SymbolFileTypeSP type_sp =
      std::make_shared<SymbolFileType>(*this, toOpaqueUid(tid));
  Variable::RangeList ranges;

  DWARFExpression location = MakeGlobalLocationExpression(
      section, offset, GetObjectFile()->GetModule());

  std::string global_name("::");
  global_name += name;
  VariableSP var_sp = std::make_shared<Variable>(
      toOpaqueUid(var_id), name.str().c_str(), global_name.c_str(), type_sp,
      scope, comp_unit.get(), ranges, &decl, location, is_external, false,
      false);
  var_sp->SetLocationIsConstantValueData(false);

  return var_sp;
}

lldb::VariableSP
SymbolFileNativePDB::CreateConstantSymbol(PdbGlobalSymId var_id,
                                          const CVSymbol &cvs) {
  TpiStream &tpi = m_index->tpi();
  ConstantSym constant(cvs.kind());

  llvm::cantFail(SymbolDeserializer::deserializeAs<ConstantSym>(cvs, constant));
  std::string global_name("::");
  global_name += constant.Name;
  PdbTypeSymId tid{constant.Type, false};
  SymbolFileTypeSP type_sp =
      std::make_shared<SymbolFileType>(*this, toOpaqueUid(tid));

  Declaration decl;
  Variable::RangeList ranges;
  ModuleSP module = GetObjectFile()->GetModule();
  DWARFExpression location =
      MakeConstantLocationExpression(constant.Type, tpi, constant, module);

  VariableSP var_sp = std::make_shared<Variable>(
      toOpaqueUid(var_id), constant.Name.str().c_str(), global_name.c_str(),
      type_sp, eValueTypeVariableGlobal, module.get(), ranges, &decl, location,
      false, false, false);
  var_sp->SetLocationIsConstantValueData(true);
  return var_sp;
}

VariableSP
SymbolFileNativePDB::GetOrCreateGlobalVariable(PdbGlobalSymId var_id) {
  auto emplace_result = m_global_vars.try_emplace(toOpaqueUid(var_id), nullptr);
  if (emplace_result.second)
    emplace_result.first->second = CreateGlobalVariable(var_id);

  return emplace_result.first->second;
}

lldb::TypeSP SymbolFileNativePDB::GetOrCreateType(TypeIndex ti) {
  return GetOrCreateType(PdbTypeSymId{ti, false});
}

FunctionSP SymbolFileNativePDB::GetOrCreateFunction(PdbCompilandSymId func_id,
                                                    const SymbolContext &sc) {
  auto emplace_result = m_functions.try_emplace(toOpaqueUid(func_id), nullptr);
  if (emplace_result.second)
    emplace_result.first->second = CreateFunction(func_id, sc);

  lldbassert(emplace_result.first->second);
  return emplace_result.first->second;
}

CompUnitSP
SymbolFileNativePDB::GetOrCreateCompileUnit(const CompilandIndexItem &cci) {

  auto emplace_result =
      m_compilands.try_emplace(toOpaqueUid(cci.m_id), nullptr);
  if (emplace_result.second)
    emplace_result.first->second = CreateCompileUnit(cci);

  lldbassert(emplace_result.first->second);
  return emplace_result.first->second;
}

lldb::CompUnitSP SymbolFileNativePDB::ParseCompileUnitAtIndex(uint32_t index) {
  if (index >= GetNumCompileUnits())
    return CompUnitSP();
  lldbassert(index < UINT16_MAX);
  if (index >= UINT16_MAX)
    return nullptr;

  CompilandIndexItem &item = m_index->compilands().GetOrCreateCompiland(index);

  return GetOrCreateCompileUnit(item);
}

lldb::LanguageType
SymbolFileNativePDB::ParseCompileUnitLanguage(const SymbolContext &sc) {
  // What fields should I expect to be filled out on the SymbolContext?  Is it
  // safe to assume that `sc.comp_unit` is valid?
  if (!sc.comp_unit)
    return lldb::eLanguageTypeUnknown;
  PdbSymUid uid(sc.comp_unit->GetID());
  lldbassert(uid.kind() == PdbSymUidKind::Compiland);

  CompilandIndexItem *item =
      m_index->compilands().GetCompiland(uid.asCompiland().modi);
  lldbassert(item);
  if (!item->m_compile_opts)
    return lldb::eLanguageTypeUnknown;

  return TranslateLanguage(item->m_compile_opts->getLanguage());
}

size_t SymbolFileNativePDB::ParseCompileUnitFunctions(const SymbolContext &sc) {
  lldbassert(sc.comp_unit);
  return false;
}

static bool NeedsResolvedCompileUnit(uint32_t resolve_scope) {
  // If any of these flags are set, we need to resolve the compile unit.
  uint32_t flags = eSymbolContextCompUnit;
  flags |= eSymbolContextVariable;
  flags |= eSymbolContextFunction;
  flags |= eSymbolContextBlock;
  flags |= eSymbolContextLineEntry;
  return (resolve_scope & flags) != 0;
}

uint32_t SymbolFileNativePDB::ResolveSymbolContext(
    const Address &addr, SymbolContextItem resolve_scope, SymbolContext &sc) {
  uint32_t resolved_flags = 0;
  lldb::addr_t file_addr = addr.GetFileAddress();

  if (NeedsResolvedCompileUnit(resolve_scope)) {
    llvm::Optional<uint16_t> modi = m_index->GetModuleIndexForVa(file_addr);
    if (!modi)
      return 0;
    CompilandIndexItem *cci = m_index->compilands().GetCompiland(*modi);
    if (!cci)
      return 0;

    sc.comp_unit = GetOrCreateCompileUnit(*cci).get();
    resolved_flags |= eSymbolContextCompUnit;
  }

  if (resolve_scope & eSymbolContextFunction) {
    lldbassert(sc.comp_unit);
    std::vector<SymbolAndUid> matches = m_index->FindSymbolsByVa(file_addr);
    for (const auto &match : matches) {
      if (match.uid.kind() != PdbSymUidKind::CompilandSym)
        continue;
      PdbCompilandSymId csid = match.uid.asCompilandSym();
      CVSymbol cvs = m_index->ReadSymbolRecord(csid);
      if (CVSymToPDBSym(cvs.kind()) != PDB_SymType::Function)
        continue;
      sc.function = GetOrCreateFunction(csid, sc).get();
    }
    resolved_flags |= eSymbolContextFunction;
  }

  if (resolve_scope & eSymbolContextLineEntry) {
    lldbassert(sc.comp_unit);
    if (auto *line_table = sc.comp_unit->GetLineTable()) {
      if (line_table->FindLineEntryByAddress(addr, sc.line_entry))
        resolved_flags |= eSymbolContextLineEntry;
    }
  }

  return resolved_flags;
}

static void AppendLineEntryToSequence(LineTable &table, LineSequence &sequence,
                                      const CompilandIndexItem &cci,
                                      lldb::addr_t base_addr,
                                      uint32_t file_number,
                                      const LineFragmentHeader &block,
                                      const LineNumberEntry &cur) {
  LineInfo cur_info(cur.Flags);

  if (cur_info.isAlwaysStepInto() || cur_info.isNeverStepInto())
    return;

  uint64_t addr = base_addr + cur.Offset;

  bool is_statement = cur_info.isStatement();
  bool is_prologue = IsFunctionPrologue(cci, addr);
  bool is_epilogue = IsFunctionEpilogue(cci, addr);

  uint32_t lno = cur_info.getStartLine();

  table.AppendLineEntryToSequence(&sequence, addr, lno, 0, file_number,
                                  is_statement, false, is_prologue, is_epilogue,
                                  false);
}

static void TerminateLineSequence(LineTable &table,
                                  const LineFragmentHeader &block,
                                  lldb::addr_t base_addr, uint32_t file_number,
                                  uint32_t last_line,
                                  std::unique_ptr<LineSequence> seq) {
  // The end is always a terminal entry, so insert it regardless.
  table.AppendLineEntryToSequence(seq.get(), base_addr + block.CodeSize,
                                  last_line, 0, file_number, false, false,
                                  false, false, true);
  table.InsertSequence(seq.release());
}

bool SymbolFileNativePDB::ParseCompileUnitLineTable(const SymbolContext &sc) {
  // Unfortunately LLDB is set up to parse the entire compile unit line table
  // all at once, even if all it really needs is line info for a specific
  // function.  In the future it would be nice if it could set the sc.m_function
  // member, and we could only get the line info for the function in question.
  lldbassert(sc.comp_unit);
  PdbSymUid cu_id(sc.comp_unit->GetID());
  lldbassert(cu_id.kind() == PdbSymUidKind::Compiland);
  CompilandIndexItem *cci =
      m_index->compilands().GetCompiland(cu_id.asCompiland().modi);
  lldbassert(cci);
  auto line_table = llvm::make_unique<LineTable>(sc.comp_unit);

  // This is basically a copy of the .debug$S subsections from all original COFF
  // object files merged together with address relocations applied.  We are
  // looking for all DEBUG_S_LINES subsections.
  for (const DebugSubsectionRecord &dssr :
       cci->m_debug_stream.getSubsectionsArray()) {
    if (dssr.kind() != DebugSubsectionKind::Lines)
      continue;

    DebugLinesSubsectionRef lines;
    llvm::BinaryStreamReader reader(dssr.getRecordData());
    if (auto EC = lines.initialize(reader)) {
      llvm::consumeError(std::move(EC));
      return false;
    }

    const LineFragmentHeader *lfh = lines.header();
    uint64_t virtual_addr =
        m_index->MakeVirtualAddress(lfh->RelocSegment, lfh->RelocOffset);

    const auto &checksums = cci->m_strings.checksums().getArray();
    const auto &strings = cci->m_strings.strings();
    for (const LineColumnEntry &group : lines) {
      // Indices in this structure are actually offsets of records in the
      // DEBUG_S_FILECHECKSUMS subsection.  Those entries then have an index
      // into the global PDB string table.
      auto iter = checksums.at(group.NameIndex);
      if (iter == checksums.end())
        continue;

      llvm::Expected<llvm::StringRef> efn =
          strings.getString(iter->FileNameOffset);
      if (!efn) {
        llvm::consumeError(efn.takeError());
        continue;
      }

      // LLDB wants the index of the file in the list of support files.
      auto fn_iter = llvm::find(cci->m_file_list, *efn);
      lldbassert(fn_iter != cci->m_file_list.end());
      uint32_t file_index = std::distance(cci->m_file_list.begin(), fn_iter);

      std::unique_ptr<LineSequence> sequence(
          line_table->CreateLineSequenceContainer());
      lldbassert(!group.LineNumbers.empty());

      for (const LineNumberEntry &entry : group.LineNumbers) {
        AppendLineEntryToSequence(*line_table, *sequence, *cci, virtual_addr,
                                  file_index, *lfh, entry);
      }
      LineInfo last_line(group.LineNumbers.back().Flags);
      TerminateLineSequence(*line_table, *lfh, virtual_addr, file_index,
                            last_line.getEndLine(), std::move(sequence));
    }
  }

  if (line_table->GetSize() == 0)
    return false;

  sc.comp_unit->SetLineTable(line_table.release());
  return true;
}

bool SymbolFileNativePDB::ParseCompileUnitDebugMacros(const SymbolContext &sc) {
  // PDB doesn't contain information about macros
  return false;
}

bool SymbolFileNativePDB::ParseCompileUnitSupportFiles(
    const SymbolContext &sc, FileSpecList &support_files) {
  lldbassert(sc.comp_unit);

  PdbSymUid cu_id(sc.comp_unit->GetID());
  lldbassert(cu_id.kind() == PdbSymUidKind::Compiland);
  CompilandIndexItem *cci =
      m_index->compilands().GetCompiland(cu_id.asCompiland().modi);
  lldbassert(cci);

  for (llvm::StringRef f : cci->m_file_list) {
    FileSpec::Style style =
        f.startswith("/") ? FileSpec::Style::posix : FileSpec::Style::windows;
    FileSpec spec(f, style);
    support_files.Append(spec);
  }

  return true;
}

bool SymbolFileNativePDB::ParseImportedModules(
    const SymbolContext &sc, std::vector<ConstString> &imported_modules) {
  // PDB does not yet support module debug info
  return false;
}

size_t SymbolFileNativePDB::ParseFunctionBlocks(const SymbolContext &sc) {
  lldbassert(sc.comp_unit && sc.function);
  return 0;
}

void SymbolFileNativePDB::DumpClangAST(Stream &s) {
  if (!m_clang)
    return;
  m_clang->Dump(s);
}

uint32_t SymbolFileNativePDB::FindGlobalVariables(
    const ConstString &name, const CompilerDeclContext *parent_decl_ctx,
    uint32_t max_matches, VariableList &variables) {
  using SymbolAndOffset = std::pair<uint32_t, llvm::codeview::CVSymbol>;

  std::vector<SymbolAndOffset> results = m_index->globals().findRecordsByName(
      name.GetStringRef(), m_index->symrecords());
  for (const SymbolAndOffset &result : results) {
    VariableSP var;
    switch (result.second.kind()) {
    case SymbolKind::S_GDATA32:
    case SymbolKind::S_LDATA32:
    case SymbolKind::S_GTHREAD32:
    case SymbolKind::S_LTHREAD32:
    case SymbolKind::S_CONSTANT: {
      PdbGlobalSymId global{result.first, false};
      var = GetOrCreateGlobalVariable(global);
      variables.AddVariable(var);
      break;
    }
    default:
      continue;
    }
  }
  return variables.GetSize();
}

uint32_t SymbolFileNativePDB::FindFunctions(
    const ConstString &name, const CompilerDeclContext *parent_decl_ctx,
    FunctionNameType name_type_mask, bool include_inlines, bool append,
    SymbolContextList &sc_list) {
  // For now we only support lookup by method name.
  if (!(name_type_mask & eFunctionNameTypeMethod))
    return 0;

  using SymbolAndOffset = std::pair<uint32_t, llvm::codeview::CVSymbol>;

  std::vector<SymbolAndOffset> matches = m_index->globals().findRecordsByName(
      name.GetStringRef(), m_index->symrecords());
  for (const SymbolAndOffset &match : matches) {
    if (match.second.kind() != S_PROCREF && match.second.kind() != S_LPROCREF)
      continue;
    ProcRefSym proc(match.second.kind());
    cantFail(SymbolDeserializer::deserializeAs<ProcRefSym>(match.second, proc));

    if (!IsValidRecord(proc))
      continue;

    CompilandIndexItem &cci =
        m_index->compilands().GetOrCreateCompiland(proc.modi());
    SymbolContext sc;

    sc.comp_unit = GetOrCreateCompileUnit(cci).get();
    sc.module_sp = sc.comp_unit->GetModule();
    PdbCompilandSymId func_id{proc.modi(), proc.SymOffset};
    sc.function = GetOrCreateFunction(func_id, sc).get();

    sc_list.Append(sc);
  }

  return sc_list.GetSize();
}

uint32_t SymbolFileNativePDB::FindFunctions(const RegularExpression &regex,
                                            bool include_inlines, bool append,
                                            SymbolContextList &sc_list) {
  return 0;
}

uint32_t SymbolFileNativePDB::FindTypes(
    const SymbolContext &sc, const ConstString &name,
    const CompilerDeclContext *parent_decl_ctx, bool append,
    uint32_t max_matches, llvm::DenseSet<SymbolFile *> &searched_symbol_files,
    TypeMap &types) {
  if (!append)
    types.Clear();
  if (!name)
    return 0;

  searched_symbol_files.clear();
  searched_symbol_files.insert(this);

  // There is an assumption 'name' is not a regex
  size_t match_count = FindTypesByName(name.GetStringRef(), max_matches, types);

  return match_count;
}

size_t
SymbolFileNativePDB::FindTypes(const std::vector<CompilerContext> &context,
                               bool append, TypeMap &types) {
  return 0;
}

size_t SymbolFileNativePDB::FindTypesByName(llvm::StringRef name,
                                            uint32_t max_matches,
                                            TypeMap &types) {

  size_t match_count = 0;
  std::vector<TypeIndex> matches = m_index->tpi().findRecordsByName(name);
  if (max_matches > 0 && max_matches < matches.size())
    matches.resize(max_matches);

  for (TypeIndex ti : matches) {
    TypeSP type = GetOrCreateType(ti);
    if (!type)
      continue;

    types.Insert(type);
    ++match_count;
  }
  return match_count;
}

size_t SymbolFileNativePDB::ParseTypes(const SymbolContext &sc) { return 0; }

Type *SymbolFileNativePDB::ResolveTypeUID(lldb::user_id_t type_uid) {
  auto iter = m_types.find(type_uid);
  // lldb should not be passing us non-sensical type uids.  the only way it
  // could have a type uid in the first place is if we handed it out, in which
  // case we should know about the type.  However, that doesn't mean we've
  // instantiated it yet.  We can vend out a UID for a future type.  So if the
  // type doesn't exist, let's instantiate it now.
  if (iter != m_types.end())
    return &*iter->second;

  PdbSymUid uid(type_uid);
  lldbassert(uid.kind() == PdbSymUidKind::Type);
  PdbTypeSymId type_id = uid.asTypeSym();
  if (type_id.index.isNoneType())
    return nullptr;

  TypeSP type_sp = CreateAndCacheType(type_id);
  return &*type_sp;
}

llvm::Optional<SymbolFile::ArrayInfo>
SymbolFileNativePDB::GetDynamicArrayInfoForUID(
    lldb::user_id_t type_uid, const lldb_private::ExecutionContext *exe_ctx) {
  return llvm::None;
}


bool SymbolFileNativePDB::CompleteType(CompilerType &compiler_type) {
  // If this is not in our map, it's an error.
  clang::TagDecl *tag_decl = m_clang->GetAsTagDecl(compiler_type);
  lldbassert(tag_decl);
  auto status_iter = m_decl_to_status.find(tag_decl);
  lldbassert(status_iter != m_decl_to_status.end());

  // If it's already complete, just return.
  DeclStatus &status = status_iter->second;
  if (status.status == Type::eResolveStateFull)
    return true;

  PdbTypeSymId type_id = PdbSymUid(status.uid).asTypeSym();

  lldbassert(IsTagRecord(type_id, m_index->tpi()));

  ClangASTContext::SetHasExternalStorage(compiler_type.GetOpaqueQualType(),
                                         false);

  // In CreateAndCacheType, we already go out of our way to resolve forward
  // ref UDTs to full decls, and the uids we vend out always refer to full
  // decls if a full decl exists in the debug info.  So if we don't have a full
  // decl here, it means one doesn't exist in the debug info, and we can't
  // complete the type.
  CVType cvt = m_index->tpi().getType(TypeIndex(type_id.index));
  if (IsForwardRefUdt(cvt))
    return false;

  auto types_iter = m_types.find(status.uid);
  lldbassert(types_iter != m_types.end());

  if (cvt.kind() == LF_MODIFIER) {
    TypeIndex unmodified_type = LookThroughModifierRecord(cvt);
    cvt = m_index->tpi().getType(unmodified_type);
    // LF_MODIFIERS usually point to forward decls, so this is the one case
    // where we won't have been able to resolve a forward decl to a full decl
    // earlier on.  So we need to do that now.
    if (IsForwardRefUdt(cvt)) {
      llvm::Expected<TypeIndex> expected_full_ti =
          m_index->tpi().findFullDeclForForwardRef(unmodified_type);
      if (!expected_full_ti) {
        llvm::consumeError(expected_full_ti.takeError());
        return false;
      }
      cvt = m_index->tpi().getType(*expected_full_ti);
      lldbassert(!IsForwardRefUdt(cvt));
      unmodified_type = *expected_full_ti;
    }
    type_id = PdbTypeSymId{unmodified_type, false};
  }
  TypeIndex field_list_ti = GetFieldListIndex(cvt);
  CVType field_list_cvt = m_index->tpi().getType(field_list_ti);
  if (field_list_cvt.kind() != LF_FIELDLIST)
    return false;

  // Visit all members of this class, then perform any finalization necessary
  // to complete the class.
  UdtRecordCompleter completer(type_id, compiler_type, *tag_decl, *this);
  auto error =
      llvm::codeview::visitMemberRecordStream(field_list_cvt.data(), completer);
  completer.complete();

  status.status = Type::eResolveStateFull;
  if (!error)
    return true;

  llvm::consumeError(std::move(error));
  return false;
}

size_t SymbolFileNativePDB::GetTypes(lldb_private::SymbolContextScope *sc_scope,
                                     TypeClass type_mask,
                                     lldb_private::TypeList &type_list) {
  return 0;
}

CompilerDeclContext
SymbolFileNativePDB::FindNamespace(const SymbolContext &sc,
                                   const ConstString &name,
                                   const CompilerDeclContext *parent_decl_ctx) {
  return {};
}

TypeSystem *
SymbolFileNativePDB::GetTypeSystemForLanguage(lldb::LanguageType language) {
  auto type_system =
      m_obj_file->GetModule()->GetTypeSystemForLanguage(language);
  if (type_system)
    type_system->SetSymbolFile(this);
  return type_system;
}

ConstString SymbolFileNativePDB::GetPluginName() {
  static ConstString g_name("pdb");
  return g_name;
}

uint32_t SymbolFileNativePDB::GetPluginVersion() { return 1; }
