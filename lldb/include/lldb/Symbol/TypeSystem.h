//===-- TypeSystem.h ------------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef liblldb_TypeSystem_h_
#define liblldb_TypeSystem_h_

#include <functional>
#include <string>
#include "lldb/lldb-private.h"
#include "lldb/Core/PluginInterface.h"
#include "lldb/Expression/Expression.h"
#include "lldb/Symbol/CompilerDeclContext.h"
#include "llvm/Support/Casting.h"

class DWARFDIE;
class DWARFASTParser;

namespace lldb_private {
    
//----------------------------------------------------------------------
// Interface for representing the Type Systems in different languages.
//----------------------------------------------------------------------
class TypeSystem : public PluginInterface
{
public:
    //----------------------------------------------------------------------
    // Intrusive type system that allows us to use llvm casting.
    //
    // To add a new type system:
    //
    // 1 - Add a new enumeration for llvm casting below for your TypeSystem
    //     subclass, here we will use eKindFoo
    //
    // 2 - Your TypeSystem subclass will inherit from TypeSystem and needs
    //     to implement a static classof() function that returns your
    //     enumeration:
    //
    //    class Foo : public lldb_private::TypeSystem
    //    {
    //        static bool classof(const TypeSystem *ts)
    //        {
    //            return ts->getKind() == TypeSystem::eKindFoo;
    //        }
    //    };
    //
    // 3 - Contruct your TypeSystem subclass with the enumeration from below
    //
    //    Foo() :
    //        TypeSystem(TypeSystem::eKindFoo),
    //        ...
    //    {
    //    }
    //
    // Then you can use the llvm casting on any "TypeSystem *" to get an
    // instance of your subclass.
    //----------------------------------------------------------------------
    enum LLVMCastKind {
        eKindClang,
        eKindSwift,
        eKindGo,
        kNumKinds
    };

    LLVMCastKind getKind() const { return m_kind; }

    static lldb::TypeSystemSP
    CreateInstance (lldb::LanguageType language, const lldb_private::ArchSpec &arch);

    //----------------------------------------------------------------------
    // Constructors and Destructors
    //----------------------------------------------------------------------
    TypeSystem (LLVMCastKind kind);
    
    virtual ~TypeSystem ();

    virtual DWARFASTParser *
    GetDWARFParser ()
    {
        return nullptr;
    }

    virtual SymbolFile *
    GetSymbolFile () const
    {
        return m_sym_file;
    }

    // Returns true if the symbol file changed during the set accessor.
    virtual void
    SetSymbolFile (SymbolFile *sym_file)
    {
        m_sym_file = sym_file;
    }

    //----------------------------------------------------------------------
    // CompilerDecl functions
    //----------------------------------------------------------------------
    virtual ConstString
    DeclGetName (void *opaque_decl) = 0;

    virtual lldb::VariableSP
    DeclGetVariable (void *opaque_decl) = 0;

    virtual void
    DeclLinkToObject (void *opaque_decl, std::shared_ptr<void> object) = 0;

    //----------------------------------------------------------------------
    // CompilerDeclContext functions
    //----------------------------------------------------------------------
    
    virtual std::vector<void *>
    DeclContextFindDeclByName (void *opaque_decl_ctx, ConstString name) = 0;

    virtual bool
    DeclContextIsStructUnionOrClass (void *opaque_decl_ctx) = 0;

    virtual ConstString
    DeclContextGetName (void *opaque_decl_ctx) = 0;

    virtual bool
    DeclContextIsClassMethod (void *opaque_decl_ctx,
                              lldb::LanguageType *language_ptr,
                              bool *is_instance_method_ptr,
                              ConstString *language_object_name_ptr) = 0;

    //----------------------------------------------------------------------
    // Tests
    //----------------------------------------------------------------------
    
    virtual bool
    IsArrayType (void *type,
                 CompilerType *element_type,
                 uint64_t *size,
                 bool *is_incomplete) = 0;
    
    virtual bool
    IsAggregateType (void *type) = 0;
    
    virtual bool
    IsCharType (void *type) = 0;
    
    virtual bool
    IsCompleteType (void *type) = 0;
    
    virtual bool
    IsDefined(void *type) = 0;
    
    virtual bool
    IsFloatingPointType (void *type, uint32_t &count, bool &is_complex) = 0;
    
    virtual bool
    IsFunctionType (void *type, bool *is_variadic_ptr) = 0;

    virtual size_t
    GetNumberOfFunctionArguments (void *type) = 0;
    
    virtual CompilerType
    GetFunctionArgumentAtIndex (void *type, const size_t index) = 0;
    
    virtual bool
    IsFunctionPointerType (void *type) = 0;
    
    virtual bool
    IsIntegerType (void *type, bool &is_signed) = 0;
    
    virtual bool
    IsPossibleDynamicType (void *type,
                           CompilerType *target_type, // Can pass NULL
                           bool check_cplusplus,
                           bool check_objc) = 0;
    
    virtual bool
    IsPointerType (void *type, CompilerType *pointee_type) = 0;
    
    virtual bool
    IsScalarType (void *type) = 0;
    
    virtual bool
    IsVoidType (void *type) = 0;

    // TypeSystems can support more than one language
    virtual bool
    SupportsLanguage (lldb::LanguageType language) = 0;

    //----------------------------------------------------------------------
    // Type Completion
    //----------------------------------------------------------------------
    
    virtual bool
    GetCompleteType (void *type) = 0;
    
    //----------------------------------------------------------------------
    // AST related queries
    //----------------------------------------------------------------------
    
    virtual uint32_t
    GetPointerByteSize () = 0;
    
    //----------------------------------------------------------------------
    // Accessors
    //----------------------------------------------------------------------
    
    virtual ConstString
    GetTypeName (void *type) = 0;
    
    virtual uint32_t
    GetTypeInfo (void *type, CompilerType *pointee_or_element_clang_type) = 0;
    
    virtual lldb::LanguageType
    GetMinimumLanguage (void *type) = 0;
    
    virtual lldb::TypeClass
    GetTypeClass (void *type) = 0;
    
    //----------------------------------------------------------------------
    // Creating related types
    //----------------------------------------------------------------------
    
    virtual CompilerType
    GetArrayElementType (void *type, uint64_t *stride) = 0;
    
    virtual CompilerType
    GetCanonicalType (void *type) = 0;
    
    // Returns -1 if this isn't a function of if the function doesn't have a prototype
    // Returns a value >= 0 if there is a prototype.
    virtual int
    GetFunctionArgumentCount (void *type) = 0;
    
    virtual CompilerType
    GetFunctionArgumentTypeAtIndex (void *type, size_t idx) = 0;
    
    virtual CompilerType
    GetFunctionReturnType (void *type) = 0;
    
    virtual size_t
    GetNumMemberFunctions (void *type) = 0;
    
    virtual TypeMemberFunctionImpl
    GetMemberFunctionAtIndex (void *type, size_t idx) = 0;
    
    virtual CompilerType
    GetPointeeType (void *type) = 0;
    
    virtual CompilerType
    GetPointerType (void *type) = 0;

    virtual CompilerType
    GetLValueReferenceType (void *type);

    virtual CompilerType
    GetRValueReferenceType (void *type);

    virtual CompilerType
    AddConstModifier (void *type);

    virtual CompilerType
    AddVolatileModifier (void *type);

    virtual CompilerType
    AddRestrictModifier (void *type);

    virtual CompilerType
    CreateTypedef (void *type, const char *name, const CompilerDeclContext &decl_ctx);

    //----------------------------------------------------------------------
    // Exploring the type
    //----------------------------------------------------------------------
    
    virtual uint64_t
    GetBitSize (void *type, ExecutionContextScope *exe_scope) = 0;
    
    virtual lldb::Encoding
    GetEncoding (void *type, uint64_t &count) = 0;
    
    virtual lldb::Format
    GetFormat (void *type) = 0;
    
    virtual uint32_t
    GetNumChildren (void *type, bool omit_empty_base_classes) = 0;

    virtual CompilerType
    GetBuiltinTypeByName (const ConstString &name);

    virtual lldb::BasicType
    GetBasicTypeEnumeration (void *type) = 0;

    virtual void
    ForEachEnumerator (void *type, std::function <bool (const CompilerType &integer_type, const ConstString &name, const llvm::APSInt &value)> const &callback)
    {
    }

    virtual uint32_t
    GetNumFields (void *type) = 0;
    
    virtual CompilerType
    GetFieldAtIndex (void *type,
                     size_t idx,
                     std::string& name,
                     uint64_t *bit_offset_ptr,
                     uint32_t *bitfield_bit_size_ptr,
                     bool *is_bitfield_ptr) = 0;

    virtual uint32_t
    GetNumDirectBaseClasses (void *type) = 0;

    virtual uint32_t
    GetNumVirtualBaseClasses (void *type) = 0;

    virtual CompilerType
    GetDirectBaseClassAtIndex (void *type,
                               size_t idx,
                               uint32_t *bit_offset_ptr) = 0;

    virtual CompilerType
    GetVirtualBaseClassAtIndex (void *type,
                                size_t idx,
                                uint32_t *bit_offset_ptr) = 0;

    virtual CompilerType
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
                              ValueObject *valobj) = 0;
    
    // Lookup a child given a name. This function will match base class names
    // and member member names in "clang_type" only, not descendants.
    virtual uint32_t
    GetIndexOfChildWithName (void *type,
                             const char *name,
                             bool omit_empty_base_classes) = 0;
    
    // Lookup a child member given a name. This function will match member names
    // only and will descend into "clang_type" children in search for the first
    // member in this class, or any base class that matches "name".
    // TODO: Return all matches for a given name by returning a vector<vector<uint32_t>>
    // so we catch all names that match a given child name, not just the first.
    virtual size_t
    GetIndexOfChildMemberWithName (void *type,
                                   const char *name,
                                   bool omit_empty_base_classes,
                                   std::vector<uint32_t>& child_indexes) = 0;
    
    virtual size_t
    GetNumTemplateArguments (void *type) = 0;
    
    virtual CompilerType
    GetTemplateArgument (void *type,
                         size_t idx,
                         lldb::TemplateArgumentKind &kind) = 0;
    
    //----------------------------------------------------------------------
    // Dumping types
    //----------------------------------------------------------------------
    virtual void
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
               uint32_t depth) = 0;
    
    virtual bool
    DumpTypeValue (void *type,
                   Stream *s,
                   lldb::Format format,
                   const DataExtractor &data,
                   lldb::offset_t data_offset,
                   size_t data_byte_size,
                   uint32_t bitfield_bit_size,
                   uint32_t bitfield_bit_offset,
                   ExecutionContextScope *exe_scope) = 0;
    
    virtual void
    DumpTypeDescription (void *type) = 0; // Dump to stdout
    
    virtual void
    DumpTypeDescription (void *type, Stream *s) = 0;
    
    //----------------------------------------------------------------------
    // TODO: These methods appear unused. Should they be removed?
    //----------------------------------------------------------------------

    virtual bool
    IsRuntimeGeneratedType (void *type) = 0;
    
    virtual void
    DumpSummary (void *type,
                 ExecutionContext *exe_ctx,
                 Stream *s,
                 const DataExtractor &data,
                 lldb::offset_t data_offset,
                 size_t data_byte_size) = 0;

    // Converts "s" to a floating point value and place resulting floating
    // point bytes in the "dst" buffer.
    virtual size_t
    ConvertStringToFloatValue (void *type,
                               const char *s,
                               uint8_t *dst,
                               size_t dst_size) = 0;
    
    //----------------------------------------------------------------------
    // TODO: Determine if these methods should move to ClangASTContext.
    //----------------------------------------------------------------------

    virtual bool
    IsPointerOrReferenceType (void *type, CompilerType *pointee_type) = 0;

    virtual unsigned
    GetTypeQualifiers(void *type) = 0;
    
    virtual bool
    IsCStringType (void *type, uint32_t &length) = 0;
    
    virtual size_t
    GetTypeBitAlign (void *type) = 0;
    
    virtual CompilerType
    GetBasicTypeFromAST (lldb::BasicType basic_type) = 0;
    
    virtual CompilerType
    GetBuiltinTypeForEncodingAndBitSize(lldb::Encoding encoding,
                                        size_t bit_size) = 0;

    virtual bool
    IsBeingDefined (void *type) = 0;
    
    virtual bool
    IsConst(void *type) = 0;
    
    virtual uint32_t
    IsHomogeneousAggregate (void *type, CompilerType* base_type_ptr) = 0;
    
    virtual bool
    IsPolymorphicClass (void *type) = 0;
    
    virtual bool
    IsTypedefType (void *type) = 0;
    
    // If the current object represents a typedef type, get the underlying type
    virtual CompilerType
    GetTypedefedType (void *type) = 0;

    virtual bool
    IsVectorType (void *type,
                  CompilerType *element_type,
                  uint64_t *size) = 0;
    
    virtual CompilerType
    GetFullyUnqualifiedType (void *type) = 0;
    
    virtual CompilerType
    GetNonReferenceType (void *type) = 0;
    
    virtual bool
    IsReferenceType (void *type, CompilerType *pointee_type, bool* is_rvalue) = 0;
    
    virtual UserExpression *
    GetUserExpression (const char *expr,
                       const char *expr_prefix,
                       lldb::LanguageType language,
                       Expression::ResultType desired_type)
    {
        return nullptr;
    }
    
    virtual FunctionCaller *
    GetFunctionCaller (const CompilerType &return_type,
                       const Address& function_address,
                       const ValueList &arg_value_list,
                       const char *name)
    {
        return nullptr;
    }
    
    virtual UtilityFunction *
    GetUtilityFunction(const char *text, const char *name)
    {
        return nullptr;
    }
    
protected:
    const LLVMCastKind m_kind; // Support for llvm casting
    SymbolFile *m_sym_file;

};
    
} // namespace lldb_private

#endif // #ifndef liblldb_TypeSystem_h_
