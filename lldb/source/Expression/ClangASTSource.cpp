//===-- ClangASTSource.cpp ---------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//


#include "clang/AST/ASTContext.h"
#include "lldb/Core/Log.h"
#include "lldb/Core/Module.h"
#include "lldb/Expression/ClangASTSource.h"
#include "lldb/Expression/ClangExpression.h"
#include "lldb/Expression/ClangExpressionDeclMap.h"

using namespace clang;
using namespace lldb_private;

ClangASTSource::~ClangASTSource() 
{
}

void
ClangASTSource::StartTranslationUnit(ASTConsumer *Consumer) 
{
    // Tell Sema to ask us when looking into the translation unit's decl.
    m_ast_context.getTranslationUnitDecl()->setHasExternalVisibleStorage();
    m_ast_context.getTranslationUnitDecl()->setHasExternalLexicalStorage();
}

// The core lookup interface.
DeclContext::lookup_result 
ClangASTSource::FindExternalVisibleDeclsByName
(
    const DeclContext *decl_ctx, 
    DeclarationName clang_decl_name
) 
{
    if (m_decl_map.GetImportInProgress())
        return SetNoExternalVisibleDeclsForName(decl_ctx, clang_decl_name);
        
    std::string decl_name (clang_decl_name.getAsString());

//    if (m_decl_map.DoingASTImport ())
//      return DeclContext::lookup_result();
//        
    switch (clang_decl_name.getNameKind()) {
    // Normal identifiers.
    case DeclarationName::Identifier:
        if (clang_decl_name.getAsIdentifierInfo()->getBuiltinID() != 0)
            return SetNoExternalVisibleDeclsForName(decl_ctx, clang_decl_name);
        break;
            
    // Operator names.  Not important for now.
    case DeclarationName::CXXOperatorName:
    case DeclarationName::CXXLiteralOperatorName:
      return DeclContext::lookup_result();
            
    // Using directives found in this context.
    // Tell Sema we didn't find any or we'll end up getting asked a *lot*.
    case DeclarationName::CXXUsingDirective:
      return SetNoExternalVisibleDeclsForName(decl_ctx, clang_decl_name);
            
    // These aren't looked up like this.
    case DeclarationName::ObjCZeroArgSelector:
    case DeclarationName::ObjCOneArgSelector:
    case DeclarationName::ObjCMultiArgSelector:
      return DeclContext::lookup_result();

    // These aren't possible in the global context.
    case DeclarationName::CXXConstructorName:
    case DeclarationName::CXXDestructorName:
    case DeclarationName::CXXConversionFunctionName:
      return DeclContext::lookup_result();
    }


    if (!m_decl_map.GetLookupsEnabled())
    {
        // Wait until we see a '$' at the start of a name before we start doing 
        // any lookups so we can avoid lookup up all of the builtin types.
        if (!decl_name.empty() && decl_name[0] == '$')
        {
            m_decl_map.SetLookupsEnabled ();
        }
        else
        {               
            return SetNoExternalVisibleDeclsForName(decl_ctx, clang_decl_name);
        }
    }

    ConstString const_decl_name(decl_name.c_str());
    
    const char *uniqued_const_decl_name = const_decl_name.GetCString();
    if (m_active_lookups.find (uniqued_const_decl_name) != m_active_lookups.end())
    {
        // We are currently looking up this name...
        return DeclContext::lookup_result();
    }
    m_active_lookups.insert(uniqued_const_decl_name);
//  static uint32_t g_depth = 0;
//  ++g_depth;
//  printf("[%5u] FindExternalVisibleDeclsByName() \"%s\"\n", g_depth, uniqued_const_decl_name);
    llvm::SmallVector<NamedDecl*, 4> name_decls;    
    NameSearchContext name_search_context(*this, name_decls, clang_decl_name, decl_ctx);
    m_decl_map.GetDecls(name_search_context, const_decl_name);
    DeclContext::lookup_result result (SetExternalVisibleDeclsForName (decl_ctx, clang_decl_name, name_decls));
//  --g_depth;
    m_active_lookups.erase (uniqued_const_decl_name);
    return result;
}

void
ClangASTSource::CompleteType (TagDecl *tag_decl)
{
    m_decl_map.CompleteTagDecl (tag_decl);
}

void
ClangASTSource::CompleteType (ObjCInterfaceDecl *objc_decl)
{
    m_decl_map.CompleteObjCInterfaceDecl (objc_decl);
}

void 
ClangASTSource::MaterializeVisibleDecls(const DeclContext *DC)
{
    return;
}

// This is used to support iterating through an entire lexical context,
// which isn't something the debugger should ever need to do.
clang::ExternalLoadResult
ClangASTSource::FindExternalLexicalDecls
(
    const DeclContext *DC, 
    bool (*isKindWeWant)(Decl::Kind),
    llvm::SmallVectorImpl<Decl*> &Decls
)
{
    return m_decl_map.FindExternalLexicalDecls (DC, isKindWeWant, Decls);
}

clang::ASTContext *
NameSearchContext::GetASTContext() 
{
    return &m_ast_source.m_ast_context;
}

clang::NamedDecl *
NameSearchContext::AddVarDecl(void *type) 
{
    IdentifierInfo *ii = m_decl_name.getAsIdentifierInfo();
    
    assert (type && "Type for variable must be non-NULL!");
        
    clang::NamedDecl *Decl = VarDecl::Create(m_ast_source.m_ast_context, 
                                             const_cast<DeclContext*>(m_decl_context), 
                                             SourceLocation(), 
                                             SourceLocation(),
                                             ii, 
                                             QualType::getFromOpaquePtr(type), 
                                             0, 
                                             SC_Static, 
                                             SC_Static);
    m_decls.push_back(Decl);
    
    return Decl;
}

clang::NamedDecl *
NameSearchContext::AddFunDecl (void *type) 
{
    clang::FunctionDecl *func_decl = FunctionDecl::Create (m_ast_source.m_ast_context,
                                                           const_cast<DeclContext*>(m_decl_context),
                                                           SourceLocation(),
                                                           SourceLocation(),
                                                           m_decl_name.getAsIdentifierInfo(),
                                                           QualType::getFromOpaquePtr(type),
                                                           NULL,
                                                           SC_Static,
                                                           SC_Static,
                                                           false,
                                                           true);
    
    // We have to do more than just synthesize the FunctionDecl.  We have to
    // synthesize ParmVarDecls for all of the FunctionDecl's arguments.  To do
    // this, we raid the function's FunctionProtoType for types.
    
    QualType qual_type (QualType::getFromOpaquePtr(type));
    const FunctionProtoType *func_proto_type = qual_type->getAs<FunctionProtoType>();
    
    if (func_proto_type)
    {        
        unsigned NumArgs = func_proto_type->getNumArgs();
        unsigned ArgIndex;
        
        SmallVector<ParmVarDecl *, 5> parm_var_decls;
                
        for (ArgIndex = 0; ArgIndex < NumArgs; ++ArgIndex)
        {
            QualType arg_qual_type (func_proto_type->getArgType(ArgIndex));
            
            parm_var_decls.push_back(ParmVarDecl::Create (m_ast_source.m_ast_context,
                                                          const_cast<DeclContext*>(m_decl_context),
                                                          SourceLocation(),
                                                          SourceLocation(),
                                                          NULL,
                                                          arg_qual_type,
                                                          NULL,
                                                          SC_Static,
                                                          SC_Static,
                                                          NULL));
        }
        
        func_decl->setParams(ArrayRef<ParmVarDecl*>(parm_var_decls));
    }
    
    m_decls.push_back(func_decl);
    
    return func_decl;
}

clang::NamedDecl *
NameSearchContext::AddGenericFunDecl()
{
    FunctionProtoType::ExtProtoInfo proto_info;
    
    proto_info.Variadic = true;
    
    QualType generic_function_type(m_ast_source.m_ast_context.getFunctionType (m_ast_source.m_ast_context.UnknownAnyTy,     // result
                                                                               NULL,                                        // argument types
                                                                               0,                                           // number of arguments
                                                                               proto_info));
    
    return AddFunDecl(generic_function_type.getAsOpaquePtr());
}

clang::NamedDecl *
NameSearchContext::AddTypeDecl(void *type)
{
    if (type)
    {
        QualType qual_type = QualType::getFromOpaquePtr(type);

        if (const TagType *tag_type = dyn_cast<clang::TagType>(qual_type))
        {
            TagDecl *tag_decl = tag_type->getDecl();
            
            m_decls.push_back(tag_decl);
            
            return tag_decl;
        }
        else if (const ObjCObjectType *objc_object_type = dyn_cast<clang::ObjCObjectType>(qual_type))
        {
            ObjCInterfaceDecl *interface_decl = objc_object_type->getInterface();
            
            m_decls.push_back((NamedDecl*)interface_decl);
            
            return (NamedDecl*)interface_decl;
        }
    }
    return NULL;
}

void 
NameSearchContext::AddLookupResult (clang::DeclContextLookupConstResult result)
{
    for (clang::NamedDecl * const *decl_iterator = result.first;
         decl_iterator != result.second;
         ++decl_iterator)
        m_decls.push_back (*decl_iterator);
}

void
NameSearchContext::AddNamedDecl (clang::NamedDecl *decl)
{
    m_decls.push_back (decl);
}
