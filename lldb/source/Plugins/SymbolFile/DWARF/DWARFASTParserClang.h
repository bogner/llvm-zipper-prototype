//===-- DWARFASTParserClang.h -----------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef SymbolFileDWARF_DWARFASTParserClang_h_
#define SymbolFileDWARF_DWARFASTParserClang_h_

#include "DWARFDefines.h"
#include "DWARFASTParser.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "clang/AST/CharUnits.h"

#include "lldb/Core/ClangForward.h"
#include "lldb/Core/PluginInterface.h"
#include "lldb/Symbol/ClangASTContext.h"

class DWARFDebugInfoEntry;
class DWARFDIECollection;

class DWARFASTParserClang : public DWARFASTParser
{
public:
    DWARFASTParserClang (lldb_private::ClangASTContext &ast);

    virtual ~DWARFASTParserClang ();

    lldb::TypeSP
    ParseTypeFromDWARF (const lldb_private::SymbolContext& sc,
                        const DWARFDIE &die,
                        lldb_private::Log *log,
                        bool *type_is_new_ptr) override;


    virtual lldb_private::Function *
    ParseFunctionFromDWARF (const lldb_private::SymbolContext& sc,
                            const DWARFDIE &die) override;

    virtual bool
    CompleteTypeFromDWARF (const DWARFDIE &die,
                           lldb_private::Type *type,
                           lldb_private::CompilerType &clang_type) override;

    virtual lldb_private::CompilerDecl
    GetDeclForUIDFromDWARF (const DWARFDIE &die) override;

    virtual lldb_private::CompilerDeclContext
    GetDeclContextForUIDFromDWARF (const DWARFDIE &die) override;

    virtual lldb_private::CompilerDeclContext
    GetDeclContextContainingUIDFromDWARF (const DWARFDIE &die) override;

    bool
    LayoutRecordType(const clang::RecordDecl *record_decl,
                     uint64_t &bit_size,
                     uint64_t &alignment,
                     llvm::DenseMap<const clang::FieldDecl *, uint64_t> &field_offsets,
                     llvm::DenseMap<const clang::CXXRecordDecl *, clang::CharUnits> &base_offsets,
                     llvm::DenseMap<const clang::CXXRecordDecl *, clang::CharUnits> &vbase_offsets);
protected:

    class DelayedAddObjCClassProperty;
    typedef std::vector <DelayedAddObjCClassProperty> DelayedPropertyList;

    struct LayoutInfo
    {
        LayoutInfo () :
        bit_size(0),
        alignment(0),
        field_offsets(),
        base_offsets(),
        vbase_offsets()
        {
        }
        uint64_t bit_size;
        uint64_t alignment;
        llvm::DenseMap<const clang::FieldDecl *, uint64_t> field_offsets;
        llvm::DenseMap<const clang::CXXRecordDecl *, clang::CharUnits> base_offsets;
        llvm::DenseMap<const clang::CXXRecordDecl *, clang::CharUnits> vbase_offsets;
    };

    clang::BlockDecl *
    ResolveBlockDIE (const DWARFDIE &die);

    clang::NamespaceDecl *
    ResolveNamespaceDIE (const DWARFDIE &die);

    typedef llvm::DenseMap<const clang::RecordDecl *, LayoutInfo> RecordDeclToLayoutMap;

    bool
    ParseTemplateDIE (const DWARFDIE &die,
                      lldb_private::ClangASTContext::TemplateParameterInfos &template_param_infos);
    bool
    ParseTemplateParameterInfos (const DWARFDIE &parent_die,
                                 lldb_private::ClangASTContext::TemplateParameterInfos &template_param_infos);

    size_t
    ParseChildMembers (const lldb_private::SymbolContext& sc,
                       const DWARFDIE &die,
                       lldb_private::CompilerType &class_clang_type,
                       const lldb::LanguageType class_language,
                       std::vector<clang::CXXBaseSpecifier *>& base_classes,
                       std::vector<int>& member_accessibilities,
                       DWARFDIECollection& member_function_dies,
                       DelayedPropertyList& delayed_properties,
                       lldb::AccessType &default_accessibility,
                       bool &is_a_class,
                       LayoutInfo &layout_info);

    size_t
    ParseChildParameters (const lldb_private::SymbolContext& sc,
                          clang::DeclContext *containing_decl_ctx,
                          const DWARFDIE &parent_die,
                          bool skip_artificial,
                          bool &is_static,
                          bool &is_variadic,
                          std::vector<lldb_private::CompilerType>& function_args,
                          std::vector<clang::ParmVarDecl*>& function_param_decls,
                          unsigned &type_quals);


    void
    ParseChildArrayInfo (const lldb_private::SymbolContext& sc,
                         const DWARFDIE &parent_die,
                         int64_t& first_index,
                         std::vector<uint64_t>& element_orders,
                         uint32_t& byte_stride,
                         uint32_t& bit_stride);


    size_t
    ParseChildEnumerators (const lldb_private::SymbolContext& sc,
                           lldb_private::CompilerType &clang_type,
                           bool is_signed,
                           uint32_t enumerator_byte_size,
                           const DWARFDIE &parent_die);

    lldb_private::Type *
    GetTypeForDIE (const DWARFDIE &die);

    clang::Decl *
    GetClangDeclForDIE (const DWARFDIE &die);

    clang::DeclContext *
    GetClangDeclContextForDIE (const DWARFDIE &die);

    clang::DeclContext *
    GetClangDeclContextContainingDIE (const DWARFDIE &die,
                                      DWARFDIE *decl_ctx_die);

    bool
    CopyUniqueClassMethodTypes (const DWARFDIE &src_class_die,
                                const DWARFDIE &dst_class_die,
                                lldb_private::Type *class_type,
                                DWARFDIECollection &failures);

    clang::DeclContext *
    GetCachedClangDeclContextForDIE (const DWARFDIE &die);

    void
    LinkDeclContextToDIE (clang::DeclContext *decl_ctx,
                          const DWARFDIE &die);

    void
    LinkDeclToDIE (clang::Decl *decl, const DWARFDIE &die);

    typedef llvm::SmallPtrSet<const DWARFDebugInfoEntry *, 4> DIEPointerSet;
    typedef llvm::DenseMap<const DWARFDebugInfoEntry *, clang::DeclContext *> DIEToDeclContextMap;
    typedef llvm::DenseMap<const clang::DeclContext *, DIEPointerSet> DeclContextToDIEMap;
    typedef llvm::DenseMap<const DWARFDebugInfoEntry *, clang::Decl *> DIEToDeclMap;
    typedef llvm::DenseMap<const clang::Decl *, DIEPointerSet> DeclToDIEMap;

    lldb_private::ClangASTContext &m_ast;
    DIEToDeclMap m_die_to_decl;
    DeclToDIEMap m_decl_to_die;
    DIEToDeclContextMap m_die_to_decl_ctx;
    DeclContextToDIEMap m_decl_ctx_to_die;
    RecordDeclToLayoutMap m_record_decl_to_layout_map;
};

#endif  // SymbolFileDWARF_DWARFASTParserClang_h_
