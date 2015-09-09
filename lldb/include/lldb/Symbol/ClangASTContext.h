//===-- ClangASTContext.h ---------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef liblldb_ClangASTContext_h_
#define liblldb_ClangASTContext_h_

// C Includes
#include <stdint.h>

// C++ Includes
#include <initializer_list>
#include <string>
#include <vector>
#include <utility>

// Other libraries and framework includes
#include "llvm/ADT/SmallVector.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/TemplateBase.h"

// Project includes
#include "lldb/lldb-enumerations.h"
#include "lldb/Core/ClangForward.h"
#include "lldb/Core/ConstString.h"
#include "lldb/Symbol/CompilerType.h"
#include "lldb/Symbol/TypeSystem.h"

namespace lldb_private {

class Declaration;

class ClangASTContext : public TypeSystem
{
public:
    typedef void (*CompleteTagDeclCallback)(void *baton, clang::TagDecl *);
    typedef void (*CompleteObjCInterfaceDeclCallback)(void *baton, clang::ObjCInterfaceDecl *);

    //------------------------------------------------------------------
    // llvm casting support
    //------------------------------------------------------------------
    static bool classof(const TypeSystem *ts)
    {
        return ts->getKind() == TypeSystem::eKindClang;
    }

    //------------------------------------------------------------------
    // Constructors and Destructors
    //------------------------------------------------------------------
    ClangASTContext (const char *triple = NULL);

    ~ClangASTContext() override;
    
    static ClangASTContext*
    GetASTContext (clang::ASTContext* ast_ctx);

    clang::ASTContext *
    getASTContext();
    
    void setASTContext(clang::ASTContext* ast_ctx);
    
    clang::Builtin::Context *
    getBuiltinContext();

    clang::IdentifierTable *
    getIdentifierTable();

    clang::LangOptions *
    getLanguageOptions();

    clang::SelectorTable *
    getSelectorTable();

    clang::FileManager *
    getFileManager();
    
    clang::SourceManager *
    getSourceManager();

    clang::DiagnosticsEngine *
    getDiagnosticsEngine();
    
    clang::DiagnosticConsumer *
    getDiagnosticConsumer();

    std::shared_ptr<clang::TargetOptions> &getTargetOptions();

    clang::TargetInfo *
    getTargetInfo();

    void
    Clear();

    const char *
    GetTargetTriple ();

    void
    SetTargetTriple (const char *target_triple);

    void
    SetArchitecture (const ArchSpec &arch);

    bool
    HasExternalSource ();

    void
    SetExternalSource (llvm::IntrusiveRefCntPtr<clang::ExternalASTSource> &ast_source_ap);

    void
    RemoveExternalSource ();
    
    bool
    GetCompleteDecl (clang::Decl *decl)
    {
        return ClangASTContext::GetCompleteDecl(getASTContext(), decl);
    }
    
    static bool
    GetCompleteDecl (clang::ASTContext *ast,
                     clang::Decl *decl);

    void SetMetadataAsUserID (const void *object,
                              lldb::user_id_t user_id);

    void SetMetadata (const void *object,
                      ClangASTMetadata &meta_data)
    {
        SetMetadata(getASTContext(), object, meta_data);
    }
    
    static void
    SetMetadata (clang::ASTContext *ast,
                 const void *object,
                 ClangASTMetadata &meta_data);
    
    ClangASTMetadata *
    GetMetadata (const void *object)
    {
        return GetMetadata(getASTContext(), object);
    }
    
    static ClangASTMetadata *
    GetMetadata (clang::ASTContext *ast,
                 const void *object);
    
    //------------------------------------------------------------------
    // Basic Types
    //------------------------------------------------------------------
    CompilerType
    GetBuiltinTypeForEncodingAndBitSize (lldb::Encoding encoding,
                                          uint32_t bit_size);

    static CompilerType
    GetBuiltinTypeForEncodingAndBitSize (clang::ASTContext *ast,
                                         lldb::Encoding encoding,
                                         uint32_t bit_size);

    CompilerType
    GetBasicType (lldb::BasicType type);

    static CompilerType
    GetBasicType (clang::ASTContext *ast, lldb::BasicType type);
    
    static CompilerType
    GetBasicType (clang::ASTContext *ast, const ConstString &name);
    
    static lldb::BasicType
    GetBasicTypeEnumeration (const ConstString &name);

    CompilerType
    GetBuiltinTypeForDWARFEncodingAndBitSize (
        const char *type_name,
        uint32_t dw_ate,
        uint32_t bit_size);

    CompilerType
    GetCStringType(bool is_const);
    
    static CompilerType
    GetUnknownAnyType(clang::ASTContext *ast);
    
    CompilerType
    GetUnknownAnyType()
    {
        return ClangASTContext::GetUnknownAnyType(getASTContext());
    }
    
    
    static clang::DeclContext *
    GetDeclContextForType (clang::QualType type);

    static clang::DeclContext *
    GetDeclContextForType (const CompilerType& type);
    
    uint32_t
    GetPointerByteSize () override;

    static clang::DeclContext *
    GetTranslationUnitDecl (clang::ASTContext *ast);
    
    clang::DeclContext *
    GetTranslationUnitDecl ()
    {
        return GetTranslationUnitDecl (getASTContext());
    }
    
    static clang::Decl *
    CopyDecl (clang::ASTContext *dest_context, 
              clang::ASTContext *source_context,
              clang::Decl *source_decl);

    static bool
    AreTypesSame(CompilerType type1,
                 CompilerType type2,
                 bool ignore_qualifiers = false);
    
    static CompilerType
    GetTypeForDecl (clang::NamedDecl *decl);
    
    static CompilerType
    GetTypeForDecl (clang::TagDecl *decl);
    
    static CompilerType
    GetTypeForDecl (clang::ObjCInterfaceDecl *objc_decl);
    
    template <typename RecordDeclType>
    CompilerType
    GetTypeForIdentifier (const ConstString &type_name)
    {
        CompilerType clang_type;
        
        if (type_name.GetLength())
        {
            clang::ASTContext *ast = getASTContext();
            if (ast)
            {
                clang::IdentifierInfo &myIdent = ast->Idents.get(type_name.GetCString());
                clang::DeclarationName myName = ast->DeclarationNames.getIdentifier(&myIdent);
                
                clang::DeclContext::lookup_result result = ast->getTranslationUnitDecl()->lookup(myName);
                
                if (!result.empty())
                {
                    clang::NamedDecl *named_decl = result[0];
                    if (const RecordDeclType *record_decl = llvm::dyn_cast<RecordDeclType>(named_decl))
                        clang_type.SetCompilerType(ast, clang::QualType(record_decl->getTypeForDecl(), 0));
                }
            }
        }
        
        return clang_type;
    }
    
    CompilerType
    GetOrCreateStructForIdentifier (const ConstString &type_name,
                                    const std::initializer_list< std::pair < const char *, CompilerType > >& type_fields,
                                    bool packed = false);

    //------------------------------------------------------------------
    // Structure, Unions, Classes
    //------------------------------------------------------------------

    static clang::AccessSpecifier
    ConvertAccessTypeToAccessSpecifier (lldb::AccessType access);

    static clang::AccessSpecifier
    UnifyAccessSpecifiers (clang::AccessSpecifier lhs, clang::AccessSpecifier rhs);

    static uint32_t
    GetNumBaseClasses (const clang::CXXRecordDecl *cxx_record_decl,
                       bool omit_empty_base_classes);

    CompilerType
    CreateRecordType (clang::DeclContext *decl_ctx,
                      lldb::AccessType access_type,
                      const char *name,
                      int kind,
                      lldb::LanguageType language,
                      ClangASTMetadata *metadata = NULL);
    
    class TemplateParameterInfos
    {
    public:
        bool
        IsValid() const
        {
            if (args.empty())
                return false;
            return args.size() == names.size();
        }

        size_t
        GetSize () const
        {
            if (IsValid())
                return args.size();
            return 0;
        }

        llvm::SmallVector<const char *, 2> names;
        llvm::SmallVector<clang::TemplateArgument, 2> args;
    };

    clang::FunctionTemplateDecl *
    CreateFunctionTemplateDecl (clang::DeclContext *decl_ctx,
                                clang::FunctionDecl *func_decl,
                                const char *name, 
                                const TemplateParameterInfos &infos);
    
    void
    CreateFunctionTemplateSpecializationInfo (clang::FunctionDecl *func_decl, 
                                              clang::FunctionTemplateDecl *Template,
                                              const TemplateParameterInfos &infos);

    clang::ClassTemplateDecl *
    CreateClassTemplateDecl (clang::DeclContext *decl_ctx,
                             lldb::AccessType access_type,
                             const char *class_name, 
                             int kind, 
                             const TemplateParameterInfos &infos);

    clang::ClassTemplateSpecializationDecl *
    CreateClassTemplateSpecializationDecl (clang::DeclContext *decl_ctx,
                                           clang::ClassTemplateDecl *class_template_decl,
                                           int kind,
                                           const TemplateParameterInfos &infos);

    CompilerType
    CreateClassTemplateSpecializationType (clang::ClassTemplateSpecializationDecl *class_template_specialization_decl);

    static clang::DeclContext *
    GetAsDeclContext (clang::CXXMethodDecl *cxx_method_decl);

    static clang::DeclContext *
    GetAsDeclContext (clang::ObjCMethodDecl *objc_method_decl);

    
    static bool
    CheckOverloadedOperatorKindParameterCount (uint32_t op_kind, 
                                               uint32_t num_params);

    bool
    FieldIsBitfield (clang::FieldDecl* field,
                     uint32_t& bitfield_bit_size);

    static bool
    FieldIsBitfield (clang::ASTContext *ast,
                     clang::FieldDecl* field,
                     uint32_t& bitfield_bit_size);

    static bool
    RecordHasFields (const clang::RecordDecl *record_decl);

    CompilerType
    CreateObjCClass (const char *name, 
                     clang::DeclContext *decl_ctx, 
                     bool isForwardDecl, 
                     bool isInternal,
                     ClangASTMetadata *metadata = NULL);
    
    bool
    SetTagTypeKind (clang::QualType type, int kind) const;
    
    bool
    SetDefaultAccessForRecordFields (clang::RecordDecl* record_decl,
                                     int default_accessibility,
                                     int *assigned_accessibilities,
                                     size_t num_assigned_accessibilities);

    // Returns a mask containing bits from the ClangASTContext::eTypeXXX enumerations


    //------------------------------------------------------------------
    // Namespace Declarations
    //------------------------------------------------------------------

    clang::NamespaceDecl *
    GetUniqueNamespaceDeclaration (const char *name,
                                   clang::DeclContext *decl_ctx);

    //------------------------------------------------------------------
    // Function Types
    //------------------------------------------------------------------

    clang::FunctionDecl *
    CreateFunctionDeclaration (clang::DeclContext *decl_ctx,
                               const char *name,
                               const CompilerType &function_Type,
                               int storage,
                               bool is_inline);
    
    static CompilerType
    CreateFunctionType (clang::ASTContext *ast,
                        const CompilerType &result_type,
                        const CompilerType *args,
                        unsigned num_args,
                        bool is_variadic,
                        unsigned type_quals);
    
    CompilerType
    CreateFunctionType (const CompilerType &result_type,
                        const CompilerType *args,
                        unsigned num_args,
                        bool is_variadic,
                        unsigned type_quals)
    {
        return ClangASTContext::CreateFunctionType(getASTContext(),
                                                   result_type,
                                                   args,
                                                   num_args,
                                                   is_variadic,
                                                   type_quals);
    }
    
    clang::ParmVarDecl *
    CreateParameterDeclaration (const char *name,
                                const CompilerType &param_type,
                                int storage);

    void
    SetFunctionParameters (clang::FunctionDecl *function_decl,
                           clang::ParmVarDecl **params,
                           unsigned num_params);

    //------------------------------------------------------------------
    // Array Types
    //------------------------------------------------------------------

    CompilerType
    CreateArrayType (const CompilerType &element_type,
                     size_t element_count,
                     bool is_vector);

    //------------------------------------------------------------------
    // Enumeration Types
    //------------------------------------------------------------------
    CompilerType
    CreateEnumerationType (const char *name, 
                           clang::DeclContext *decl_ctx, 
                           const Declaration &decl, 
                           const CompilerType &integer_qual_type);
    
    //------------------------------------------------------------------
    // Integer type functions
    //------------------------------------------------------------------
    
    CompilerType
    GetIntTypeFromBitSize (size_t bit_size, bool is_signed) override
    {
        return GetIntTypeFromBitSize (getASTContext(), bit_size, is_signed);
    }
    
    static CompilerType
    GetIntTypeFromBitSize (clang::ASTContext *ast,
                           size_t bit_size, bool is_signed);
    
    CompilerType
    GetPointerSizedIntType (bool is_signed)
    {
        return GetPointerSizedIntType (getASTContext(), is_signed);
    }
    
    static CompilerType
    GetPointerSizedIntType (clang::ASTContext *ast, bool is_signed);
    
    //------------------------------------------------------------------
    // Floating point functions
    //------------------------------------------------------------------
    
    CompilerType
    GetFloatTypeFromBitSize (size_t bit_size) override
    {
        return GetFloatTypeFromBitSize (getASTContext(), bit_size);
    }

    static CompilerType
    GetFloatTypeFromBitSize (clang::ASTContext *ast,
                             size_t bit_size);

    //------------------------------------------------------------------
    // TypeSystem methods
    //------------------------------------------------------------------
    
    DWARFASTParser *
    GetDWARFParser () override;

    //------------------------------------------------------------------
    // ClangASTContext callbacks for external source lookups.
    //------------------------------------------------------------------
    static void
    CompleteTagDecl (void *baton, clang::TagDecl *);

    static void
    CompleteObjCInterfaceDecl (void *baton, clang::ObjCInterfaceDecl *);

    static bool
    LayoutRecordType(void *baton,
                     const clang::RecordDecl *record_decl,
                     uint64_t &size,
                     uint64_t &alignment,
                     llvm::DenseMap<const clang::FieldDecl *, uint64_t> &field_offsets,
                     llvm::DenseMap<const clang::CXXRecordDecl *, clang::CharUnits> &base_offsets,
                     llvm::DenseMap<const clang::CXXRecordDecl *, clang::CharUnits> &vbase_offsets);

    //----------------------------------------------------------------------
    // CompilerDeclContext override functions
    //----------------------------------------------------------------------

    bool
    DeclContextIsStructUnionOrClass (void *opaque_decl_ctx) override;

    ConstString
    DeclContextGetName (void *opaque_decl_ctx) override;

    bool
    DeclContextIsClassMethod (void *opaque_decl_ctx,
                              lldb::LanguageType *language_ptr,
                              bool *is_instance_method_ptr,
                              ConstString *language_object_name_ptr) override;

    //----------------------------------------------------------------------
    // Clang specific CompilerType predicates
    //----------------------------------------------------------------------
    
    static bool
    IsClangType (const CompilerType &ct)
    {
        return llvm::dyn_cast_or_null<ClangASTContext>(ct.GetTypeSystem()) != nullptr;
    }

    //----------------------------------------------------------------------
    // Clang specific clang::DeclContext functions
    //----------------------------------------------------------------------

    static clang::DeclContext *
    DeclContextGetAsDeclContext (const CompilerDeclContext &dc);

    static clang::ObjCMethodDecl *
    DeclContextGetAsObjCMethodDecl (const CompilerDeclContext &dc);

    static clang::CXXMethodDecl *
    DeclContextGetAsCXXMethodDecl (const CompilerDeclContext &dc);

    static clang::FunctionDecl *
    DeclContextGetAsFunctionDecl (const CompilerDeclContext &dc);

    static clang::NamespaceDecl *
    DeclContextGetAsNamespaceDecl (const CompilerDeclContext &dc);

    static ClangASTMetadata *
    DeclContextGetMetaData (const CompilerDeclContext &dc, const void *object);

    static clang::ASTContext *
    DeclContextGetClangASTContext (const CompilerDeclContext &dc);

    //----------------------------------------------------------------------
    // Tests
    //----------------------------------------------------------------------
    
    bool
    IsArrayType (void *type,
                 CompilerType *element_type,
                 uint64_t *size,
                 bool *is_incomplete) override;
    
    bool
    IsVectorType (void *type,
                  CompilerType *element_type,
                  uint64_t *size) override;
    
    bool
    IsAggregateType (void *type) override;
    
    bool
    IsBeingDefined (void *type) override;
    
    bool
    IsCharType (void *type) override;
    
    bool
    IsCompleteType (void *type) override;
    
    bool
    IsConst(void *type) override;
    
    bool
    IsCStringType (void *type, uint32_t &length) override;
    
    static bool
    IsCXXClassType (const CompilerType& type);
    
    bool
    IsDefined(void *type) override;
    
    bool
    IsFloatingPointType (void *type, uint32_t &count, bool &is_complex) override;
    
    bool
    IsFunctionType (void *type, bool *is_variadic_ptr) override;

    uint32_t
    IsHomogeneousAggregate (void *type, CompilerType* base_type_ptr) override;
    
    size_t
    GetNumberOfFunctionArguments (void *type) override;
    
    CompilerType
    GetFunctionArgumentAtIndex (void *type, const size_t index) override;
    
    bool
    IsFunctionPointerType (void *type) override;
    
    bool
    IsIntegerType (void *type, bool &is_signed) override;
    
    static bool
    IsObjCClassType (const CompilerType& type);
    
    static bool
    IsObjCClassTypeAndHasIVars (const CompilerType& type, bool check_superclass);
    
    static bool
    IsObjCObjectOrInterfaceType (const CompilerType& type);
    
    static bool
    IsObjCObjectPointerType (const CompilerType& type, CompilerType *target_type = NULL);
    
    bool
    IsPolymorphicClass (void *type) override;
    
    bool
    IsPossibleDynamicType (void *type,
                           CompilerType *target_type, // Can pass NULL
                           bool check_cplusplus,
                           bool check_objc) override;
    
    bool
    IsRuntimeGeneratedType (void *type) override;
    
    bool
    IsPointerType (void *type, CompilerType *pointee_type) override;
    
    bool
    IsPointerOrReferenceType (void *type, CompilerType *pointee_type) override;
    
    bool
    IsReferenceType (void *type, CompilerType *pointee_type, bool* is_rvalue) override;
    
    bool
    IsScalarType (void *type) override;
    
    bool
    IsTypedefType (void *type) override;
    
    bool
    IsVoidType (void *type) override;
    
    static bool
    GetCXXClassName (const CompilerType& type, std::string &class_name);
    
    static bool
    GetObjCClassName (const CompilerType& type, std::string &class_name);
    
    //----------------------------------------------------------------------
    // Type Completion
    //----------------------------------------------------------------------
    
    bool
    GetCompleteType (void *type) override;
    
    //----------------------------------------------------------------------
    // Accessors
    //----------------------------------------------------------------------
    
    ConstString
    GetTypeName (void *type) override;
    
    uint32_t
    GetTypeInfo (void *type, CompilerType *pointee_or_element_clang_type) override;
    
    lldb::LanguageType
    GetMinimumLanguage (void *type) override;
    
    lldb::TypeClass
    GetTypeClass (void *type) override;
    
    unsigned
    GetTypeQualifiers(void *type) override;
    
    //----------------------------------------------------------------------
    // Creating related types
    //----------------------------------------------------------------------
    
    static CompilerType
    AddConstModifier (const CompilerType& type);
    
    static CompilerType
    AddRestrictModifier (const CompilerType& type);
    
    static CompilerType
    AddVolatileModifier (const CompilerType& type);
    
    // Using the current type, create a new typedef to that type using "typedef_name"
    // as the name and "decl_ctx" as the decl context.
    static CompilerType
    CreateTypedefType (const CompilerType& type,
                       const char *typedef_name,
                       const CompilerDeclContext &compiler_decl_ctx);
    
    CompilerType
    GetArrayElementType (void *type, uint64_t *stride) override;
    
    CompilerType
    GetCanonicalType (void *type) override;
    
    CompilerType
    GetFullyUnqualifiedType (void *type) override;
    
    // Returns -1 if this isn't a function of if the function doesn't have a prototype
    // Returns a value >= 0 if there is a prototype.
    int
    GetFunctionArgumentCount (void *type) override;
    
    CompilerType
    GetFunctionArgumentTypeAtIndex (void *type, size_t idx) override;
    
    CompilerType
    GetFunctionReturnType (void *type) override;
    
    size_t
    GetNumMemberFunctions (void *type) override;
    
    TypeMemberFunctionImpl
    GetMemberFunctionAtIndex (void *type, size_t idx) override;
    
    static CompilerType
    GetLValueReferenceType (const CompilerType& type);
    
    CompilerType
    GetNonReferenceType (void *type) override;
    
    CompilerType
    GetPointeeType (void *type) override;
    
    CompilerType
    GetPointerType (void *type) override;
    
    static CompilerType
    GetRValueReferenceType (const CompilerType& type);
    
    // If the current object represents a typedef type, get the underlying type
    CompilerType
    GetTypedefedType (void *type) override;

    static CompilerType
    RemoveFastQualifiers (const CompilerType& type);
    
    //----------------------------------------------------------------------
    // Create related types using the current type's AST
    //----------------------------------------------------------------------
    CompilerType
    GetBasicTypeFromAST (lldb::BasicType basic_type) override;
    
    //----------------------------------------------------------------------
    // Exploring the type
    //----------------------------------------------------------------------
    
    uint64_t
    GetByteSize (void *type, ExecutionContextScope *exe_scope)
    {
        return (GetBitSize (type, exe_scope) + 7) / 8;
    }
    
    uint64_t
    GetBitSize (void *type, ExecutionContextScope *exe_scope) override;
    
    lldb::Encoding
    GetEncoding (void *type, uint64_t &count) override;
    
    lldb::Format
    GetFormat (void *type) override;
    
    size_t
    GetTypeBitAlign (void *type) override;
    
    uint32_t
    GetNumChildren (void *type, bool omit_empty_base_classes) override;
    
    lldb::BasicType
    GetBasicTypeEnumeration (void *type) override;
    
    static lldb::BasicType
    GetBasicTypeEnumeration (void *type, const ConstString &name);

    void
    ForEachEnumerator (void *type, std::function <bool (const CompilerType &integer_type, const ConstString &name, const llvm::APSInt &value)> const &callback) override;

    uint32_t
    GetNumFields (void *type) override;
    
    CompilerType
    GetFieldAtIndex (void *type,
                     size_t idx,
                     std::string& name,
                     uint64_t *bit_offset_ptr,
                     uint32_t *bitfield_bit_size_ptr,
                     bool *is_bitfield_ptr) override;

    uint32_t
    GetNumDirectBaseClasses (void *type) override;

    uint32_t
    GetNumVirtualBaseClasses (void *type) override;

    CompilerType
    GetDirectBaseClassAtIndex (void *type,
                               size_t idx,
                               uint32_t *bit_offset_ptr) override;

    CompilerType
    GetVirtualBaseClassAtIndex (void *type,
                                size_t idx,
                                uint32_t *bit_offset_ptr) override;

    static uint32_t
    GetNumPointeeChildren (clang::QualType type);
    
    CompilerType
    GetChildClangTypeAtIndex (void *type,
                              ExecutionContext *exe_ctx,
                              size_t idx,
                              bool transparent_pointers,
                              bool omit_empty_base_classes,
                              bool ignore_array_bounds,
                              std::string& child_name,
                              uint32_t &child_byte_size,
                              int32_t &child_byte_offset,
                              uint32_t &child_bitfield_bit_size,
                              uint32_t &child_bitfield_bit_offset,
                              bool &child_is_base_class,
                              bool &child_is_deref_of_parent,
                              ValueObject *valobj) override;
    
    // Lookup a child given a name. This function will match base class names
    // and member member names in "clang_type" only, not descendants.
    uint32_t
    GetIndexOfChildWithName (void *type,
                             const char *name,
                             bool omit_empty_base_classes) override;
    
    // Lookup a child member given a name. This function will match member names
    // only and will descend into "clang_type" children in search for the first
    // member in this class, or any base class that matches "name".
    // TODO: Return all matches for a given name by returning a vector<vector<uint32_t>>
    // so we catch all names that match a given child name, not just the first.
    size_t
    GetIndexOfChildMemberWithName (void *type,
                                   const char *name,
                                   bool omit_empty_base_classes,
                                   std::vector<uint32_t>& child_indexes) override;
    
    size_t
    GetNumTemplateArguments (void *type) override;
    
    CompilerType
    GetTemplateArgument (void *type,
                         size_t idx,
                         lldb::TemplateArgumentKind &kind) override;
    
    //----------------------------------------------------------------------
    // Modifying RecordType
    //----------------------------------------------------------------------
    static clang::FieldDecl *
    AddFieldToRecordType (const CompilerType& type,
                          const char *name,
                          const CompilerType &field_type,
                          lldb::AccessType access,
                          uint32_t bitfield_bit_size);
    
    static void
    BuildIndirectFields (const CompilerType& type);
    
    static void
    SetIsPacked (const CompilerType& type);
    
    static clang::VarDecl *
    AddVariableToRecordType (const CompilerType& type,
                             const char *name,
                             const CompilerType &var_type,
                             lldb::AccessType access);
    
    clang::CXXMethodDecl *
    AddMethodToCXXRecordType (void *type,
                              const char *name,
                              const CompilerType &method_type,
                              lldb::AccessType access,
                              bool is_virtual,
                              bool is_static,
                              bool is_inline,
                              bool is_explicit,
                              bool is_attr_used,
                              bool is_artificial);
    
    // C++ Base Classes
    clang::CXXBaseSpecifier *
    CreateBaseClassSpecifier (void *type,
                              lldb::AccessType access,
                              bool is_virtual,
                              bool base_of_class);
    
    static void
    DeleteBaseClassSpecifiers (clang::CXXBaseSpecifier **base_classes,
                               unsigned num_base_classes);
    
    bool
    SetBaseClassesForClassType (void *type,
                                clang::CXXBaseSpecifier const * const *base_classes,
                                unsigned num_base_classes);
    
    static bool
    SetObjCSuperClass (const CompilerType& type,
                       const CompilerType &superclass_clang_type);
    
    static bool
    AddObjCClassProperty (const CompilerType& type,
                          const char *property_name,
                          const CompilerType &property_clang_type,
                          clang::ObjCIvarDecl *ivar_decl,
                          const char *property_setter_name,
                          const char *property_getter_name,
                          uint32_t property_attributes,
                          ClangASTMetadata *metadata);
    
    static clang::ObjCMethodDecl *
    AddMethodToObjCObjectType (const CompilerType& type,
                               const char *name,  // the full symbol name as seen in the symbol table (void *type, "-[NString stringWithCString:]")
                               const CompilerType &method_clang_type,
                               lldb::AccessType access,
                               bool is_artificial);
    
    bool
    SetHasExternalStorage (void *type, bool has_extern);
    
    
    //------------------------------------------------------------------
    // Tag Declarations
    //------------------------------------------------------------------
    static bool
    StartTagDeclarationDefinition (const CompilerType &type);
    
    static bool
    CompleteTagDeclarationDefinition (const CompilerType &type);
    
    //----------------------------------------------------------------------
    // Modifying Enumeration types
    //----------------------------------------------------------------------
    bool
    AddEnumerationValueToEnumerationType (void *type,
                                          const CompilerType &enumerator_qual_type,
                                          const Declaration &decl,
                                          const char *name,
                                          int64_t enum_value,
                                          uint32_t enum_value_bit_size);
    
    
    
    CompilerType
    GetEnumerationIntegerType (void *type);
    
    //------------------------------------------------------------------
    // Pointers & References
    //------------------------------------------------------------------
    
    // Call this function using the class type when you want to make a
    // member pointer type to pointee_type.
    static CompilerType
    CreateMemberPointerType (const CompilerType& type, const CompilerType &pointee_type);
    
    
    // Converts "s" to a floating point value and place resulting floating
    // point bytes in the "dst" buffer.
    size_t
    ConvertStringToFloatValue (void *type,
                               const char *s,
                               uint8_t *dst,
                               size_t dst_size) override;
    //----------------------------------------------------------------------
    // Dumping types
    //----------------------------------------------------------------------
    void
    DumpValue (void *type,
               ExecutionContext *exe_ctx,
               Stream *s,
               lldb::Format format,
               const DataExtractor &data,
               lldb::offset_t data_offset,
               size_t data_byte_size,
               uint32_t bitfield_bit_size,
               uint32_t bitfield_bit_offset,
               bool show_types,
               bool show_summary,
               bool verbose,
               uint32_t depth) override;
    
    bool
    DumpTypeValue (void *type,
                   Stream *s,
                   lldb::Format format,
                   const DataExtractor &data,
                   lldb::offset_t data_offset,
                   size_t data_byte_size,
                   uint32_t bitfield_bit_size,
                   uint32_t bitfield_bit_offset,
                   ExecutionContextScope *exe_scope) override;
    
    void
    DumpSummary (void *type,
                 ExecutionContext *exe_ctx,
                 Stream *s,
                 const DataExtractor &data,
                 lldb::offset_t data_offset,
                 size_t data_byte_size) override;
    
    virtual void
    DumpTypeDescription (void *type) override; // Dump to stdout
    
    void
    DumpTypeDescription (void *type, Stream *s) override;
    
    static clang::EnumDecl *
    GetAsEnumDecl (const CompilerType& type);
    
    static clang::RecordDecl *
    GetAsRecordDecl (const CompilerType& type);
    
    clang::CXXRecordDecl *
    GetAsCXXRecordDecl (void *type);
    
    static clang::ObjCInterfaceDecl *
    GetAsObjCInterfaceDecl (const CompilerType& type);
    
    static clang::QualType
    GetQualType (const CompilerType& type)
    {
        // Make sure we have a clang type before making a clang::QualType
        ClangASTContext *ast = llvm::dyn_cast_or_null<ClangASTContext>(type.GetTypeSystem());
        if (ast)
            return clang::QualType::getFromOpaquePtr(type.GetOpaqueQualType());
        return clang::QualType();
    }

    static clang::QualType
    GetCanonicalQualType (const CompilerType& type)
    {
        // Make sure we have a clang type before making a clang::QualType
        ClangASTContext *ast = llvm::dyn_cast_or_null<ClangASTContext>(type.GetTypeSystem());
        if (ast)
            return clang::QualType::getFromOpaquePtr(type.GetOpaqueQualType()).getCanonicalType();
        return clang::QualType();
    }

    clang::ClassTemplateDecl *
    ParseClassTemplateDecl (clang::DeclContext *decl_ctx,
                            lldb::AccessType access_type,
                            const char *parent_name,
                            int tag_decl_kind,
                            const ClangASTContext::TemplateParameterInfos &template_param_infos);

protected:
    static clang::QualType
    GetQualType (void *type)
    {
        if (type)
            return clang::QualType::getFromOpaquePtr(type);
        return clang::QualType();
    }
    
    static clang::QualType
    GetCanonicalQualType (void *type)
    {
        if (type)
            return clang::QualType::getFromOpaquePtr(type).getCanonicalType();
        return clang::QualType();
    }

    //------------------------------------------------------------------
    // Classes that inherit from ClangASTContext can see and modify these
    //------------------------------------------------------------------
    std::string                                     m_target_triple;
    std::unique_ptr<clang::ASTContext>              m_ast_ap;
    std::unique_ptr<clang::LangOptions>             m_language_options_ap;
    std::unique_ptr<clang::FileManager>             m_file_manager_ap;
    std::unique_ptr<clang::FileSystemOptions>       m_file_system_options_ap;
    std::unique_ptr<clang::SourceManager>           m_source_manager_ap;
    std::unique_ptr<clang::DiagnosticsEngine>       m_diagnostics_engine_ap;
    std::unique_ptr<clang::DiagnosticConsumer>      m_diagnostic_consumer_ap;
    std::shared_ptr<clang::TargetOptions>           m_target_options_rp;
    std::unique_ptr<clang::TargetInfo>              m_target_info_ap;
    std::unique_ptr<clang::IdentifierTable>         m_identifier_table_ap;
    std::unique_ptr<clang::SelectorTable>           m_selector_table_ap;
    std::unique_ptr<clang::Builtin::Context>        m_builtins_ap;
    std::unique_ptr<DWARFASTParser>                 m_dwarf_ast_parser_ap;
    CompleteTagDeclCallback                         m_callback_tag_decl;
    CompleteObjCInterfaceDeclCallback               m_callback_objc_decl;
    void *                                          m_callback_baton;
    uint32_t                                        m_pointer_byte_size;
    bool                                            m_ast_owned;

private:
    //------------------------------------------------------------------
    // For ClangASTContext only
    //------------------------------------------------------------------
    ClangASTContext(const ClangASTContext&);
    const ClangASTContext& operator=(const ClangASTContext&);
};

} // namespace lldb_private

#endif // liblldb_ClangASTContext_h_
