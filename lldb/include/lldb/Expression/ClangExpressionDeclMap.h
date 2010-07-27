//===-- ClangExpressionDeclMap.h --------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef liblldb_ClangExpressionDeclMap_h_
#define liblldb_ClangExpressionDeclMap_h_

// C Includes
#include <signal.h>
#include <stdint.h>

// C++ Includes
#include <vector>

// Other libraries and framework includes
// Project includes
#include "lldb/Core/ClangForward.h"
#include "lldb/Core/Value.h"
#include "lldb/Symbol/TaggedASTType.h"

namespace llvm {
    class Value;
}

namespace lldb_private {

//----------------------------------------------------------------------
// For cases in which there are multiple classes of types that are not
// interchangeable, to allow static type checking.
//----------------------------------------------------------------------
template <unsigned int C> class TaggedClangASTType : public ClangASTType
{
public:
    TaggedClangASTType (void *type, clang::ASTContext *ast_context) :
        ClangASTType(type, ast_context) { }
    
    TaggedClangASTType (const TaggedClangASTType<C> &tw) :
        ClangASTType(tw) { }
    
    TaggedClangASTType () :
        ClangASTType() { }
    
    ~TaggedClangASTType() { }
    
    const TaggedClangASTType<C> &
    operator= (const TaggedClangASTType<C> &tw)
    {
        ClangASTType::operator= (tw);
        return *this;
    }
};


class Error;
class Function;
class NameSearchContext;
class Variable;
    
class ClangExpressionDeclMap
{
public:
    ClangExpressionDeclMap(ExecutionContext *exe_ctx);
    ~ClangExpressionDeclMap();
    
    // Interface for ClangStmtVisitor
    bool GetIndexForDecl (uint32_t &index,
                          const clang::Decl *decl);
    
    // Interface for IRForTarget
    bool AddValueToStruct (llvm::Value *value,
                           const clang::NamedDecl *decl,
                           std::string &name,
                           void *type,
                           clang::ASTContext *ast_context,
                           size_t size,
                           off_t alignment);
    bool DoStructLayout ();
    bool GetStructInfo (uint32_t &num_elements,
                        size_t &size,
                        off_t &alignment);
    bool GetStructElement (const clang::NamedDecl *&decl,
                           llvm::Value *&value,
                           off_t &offset,
                           uint32_t index);
    
    // Interface for DwarfExpression
    Value *GetValueForIndex (uint32_t index);
    
    // Interface for CommandObjectExpression
    bool Materialize(ExecutionContext *exe_ctx,
                     lldb::addr_t &struct_address,
                     Error &error);
    
    bool DumpMaterializedStruct(ExecutionContext *exe_ctx,
                                Stream &s,
                                Error &error);
    
    bool Dematerialize(ExecutionContext *exe_ctx,
                       lldb_private::Value &result_value,
                       Error &error);
    
    // Interface for ClangASTSource
    void GetDecls (NameSearchContext &context,
                   const char *name);
private:
    typedef TaggedClangASTType<0> TypeFromParser;
    typedef TaggedClangASTType<1> TypeFromUser;
    
    struct Tuple
    {
        const clang::NamedDecl  *m_decl;
        TypeFromParser          m_parser_type;
        TypeFromUser            m_user_type;
        lldb_private::Value     *m_value; /* owned by ClangExpressionDeclMap */
    };
    
    struct StructMember
    {
        const clang::NamedDecl *m_decl;
        llvm::Value            *m_value;
        std::string             m_name;
        TypeFromParser          m_parser_type;
        off_t                   m_offset;
        size_t                  m_size;
        off_t                   m_alignment;
    };
    
    typedef std::vector<Tuple> TupleVector;
    typedef TupleVector::iterator TupleIterator;
    
    typedef std::vector<StructMember> StructMemberVector;
    typedef StructMemberVector::iterator StructMemberIterator;
    
    TupleVector         m_tuples;
    StructMemberVector  m_members;
    ExecutionContext   *m_exe_ctx;
    SymbolContext      *m_sym_ctx; /* owned by ClangExpressionDeclMap */
    off_t               m_struct_alignment;
    size_t              m_struct_size;
    bool                m_struct_laid_out;
    lldb::addr_t        m_allocated_area;
    lldb::addr_t        m_materialized_location;
        
    Variable *FindVariableInScope(const SymbolContext &sym_ctx,
                                  const char *name,
                                  TypeFromUser *type = NULL);
    
    Value *GetVariableValue(ExecutionContext &exe_ctx,
                            Variable *var,
                            clang::ASTContext *parser_ast_context,
                            TypeFromUser *found_type = NULL,
                            TypeFromParser *parser_type = NULL);
    
    void AddOneVariable(NameSearchContext &context, Variable *var);
    void AddOneFunction(NameSearchContext &context, Function *fun, Symbol *sym);
    
    bool DoMaterialize (bool dematerialize,
                        ExecutionContext *exe_ctx,
                        lldb_private::Value *result_value, /* must be non-NULL if D is set */
                        Error &err);

    bool DoMaterializeOneVariable(bool dematerialize,
                                  ExecutionContext &exe_ctx,
                                  const SymbolContext &sym_ctx,
                                  const char *name,
                                  TypeFromUser type,
                                  lldb::addr_t addr, 
                                  Error &err);
};
    
} // namespace lldb_private

#endif  // liblldb_ClangExpressionDeclMap_h_
