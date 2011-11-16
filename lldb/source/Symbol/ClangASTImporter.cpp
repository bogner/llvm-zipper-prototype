//===-- ClangASTImporter.cpp ------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "clang/AST/Decl.h"
#include "clang/AST/DeclObjC.h"
#include "lldb/Core/Log.h"
#include "lldb/Core/Module.h"
#include "lldb/Symbol/ClangASTContext.h"
#include "lldb/Symbol/ClangASTImporter.h"
#include "lldb/Symbol/ClangNamespaceDecl.h"

using namespace lldb_private;
using namespace clang;

clang::QualType 
ClangASTImporter::CopyType (clang::ASTContext *dst_ast,
                            clang::ASTContext *src_ast,
                            clang::QualType type)
{
    MinionSP minion_sp (GetMinion(dst_ast, src_ast));
    
    if (minion_sp)
        return minion_sp->Import(type);
    
    return QualType();
}

lldb::clang_type_t
ClangASTImporter::CopyType (clang::ASTContext *dst_ast,
                            clang::ASTContext *src_ast,
                            lldb::clang_type_t type)
{
    return CopyType (dst_ast, src_ast, QualType::getFromOpaquePtr(type)).getAsOpaquePtr();
}

clang::Decl *
ClangASTImporter::CopyDecl (clang::ASTContext *dst_ast,
                            clang::ASTContext *src_ast,
                            clang::Decl *decl)
{
    MinionSP minion_sp;
    
    minion_sp = GetMinion(dst_ast, src_ast);
    
    if (minion_sp)
    {
        clang::Decl *result = minion_sp->Import(decl);
        
        if (!result)
        {
            lldb::LogSP log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_EXPRESSIONS));

            if (log)
            {
                if (NamedDecl *named_decl = dyn_cast<NamedDecl>(decl))
                    log->Printf("  [ClangASTImporter] WARNING: Failed to import a %s '%s'", decl->getDeclKindName(), named_decl->getNameAsString().c_str());
                else
                    log->Printf("  [ClangASTImporter] WARNING: Failed to import a %s", decl->getDeclKindName());
            }
        }
        
        return result;
    }
    
    return NULL;
}

void
ClangASTImporter::CompleteTagDecl (clang::TagDecl *decl)
{
    lldb::LogSP log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_EXPRESSIONS));
    
    DeclOrigin decl_origin = GetDeclOrigin(decl);
    
    if (!decl_origin.Valid())
        return;
    
    if (!ClangASTContext::GetCompleteDecl(decl_origin.ctx, decl_origin.decl))
        return;
    
    MinionSP minion_sp (GetMinion(&decl->getASTContext(), decl_origin.ctx));
    
    if (minion_sp)
        minion_sp->ImportDefinition(decl_origin.decl);
    
    return;
}

void
ClangASTImporter::CompleteObjCInterfaceDecl (clang::ObjCInterfaceDecl *interface_decl)
{
    lldb::LogSP log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_EXPRESSIONS));
    
    DeclOrigin decl_origin = GetDeclOrigin(interface_decl);
    
    if (!decl_origin.Valid())
        return;
    
    if (!ClangASTContext::GetCompleteDecl(decl_origin.ctx, decl_origin.decl))
        return;
    
    MinionSP minion_sp (GetMinion(&interface_decl->getASTContext(), decl_origin.ctx));
    
    if (minion_sp)
        minion_sp->ImportDefinition(decl_origin.decl);
    
    return;
}

ClangASTImporter::DeclOrigin
ClangASTImporter::GetDeclOrigin(const clang::Decl *decl)
{
    ASTContextMetadataSP context_md = GetContextMetadata(&decl->getASTContext());
    
    OriginMap &origins = context_md->m_origins;
    
    OriginMap::iterator iter = origins.find(decl);
    
    if (iter != origins.end())
        return iter->second;
    else
        return DeclOrigin();
}

void 
ClangASTImporter::RegisterNamespaceMap(const clang::NamespaceDecl *decl, 
                                       NamespaceMapSP &namespace_map)
{
    ASTContextMetadataSP context_md = GetContextMetadata(&decl->getASTContext());
    
    context_md->m_namespace_maps[decl] = namespace_map;
}

ClangASTImporter::NamespaceMapSP 
ClangASTImporter::GetNamespaceMap(const clang::NamespaceDecl *decl)
{
    ASTContextMetadataSP context_md = GetContextMetadata(&decl->getASTContext());

    NamespaceMetaMap &namespace_maps = context_md->m_namespace_maps;
    
    NamespaceMetaMap::iterator iter = namespace_maps.find(decl);
    
    if (iter != namespace_maps.end())
        return iter->second;
    else
        return NamespaceMapSP();
}

void 
ClangASTImporter::BuildNamespaceMap(const clang::NamespaceDecl *decl)
{
    ASTContextMetadataSP context_md = GetContextMetadata(&decl->getASTContext());

    const DeclContext *parent_context = decl->getDeclContext();
    const NamespaceDecl *parent_namespace = dyn_cast<NamespaceDecl>(parent_context);
    NamespaceMapSP parent_map;
    
    if (parent_namespace)
        parent_map = GetNamespaceMap(parent_namespace);
    
    NamespaceMapSP new_map;
    
    new_map.reset(new NamespaceMap);
 
    if (context_md->m_map_completer)
    {
        std::string namespace_string = decl->getDeclName().getAsString();
    
        context_md->m_map_completer->CompleteNamespaceMap (new_map, ConstString(namespace_string.c_str()), parent_map);
    }
    
    RegisterNamespaceMap (decl, new_map);
}

void 
ClangASTImporter::PurgeMaps (clang::ASTContext *dst_ast)
{
    m_metadata_map.erase(dst_ast);
}

ClangASTImporter::NamespaceMapCompleter::~NamespaceMapCompleter ()
{
    return;
}

clang::Decl 
*ClangASTImporter::Minion::Imported (clang::Decl *from, clang::Decl *to)
{
    lldb::LogSP log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_EXPRESSIONS));
    
    ASTContextMetadataSP to_context_md = m_master.GetContextMetadata(&to->getASTContext());
    ASTContextMetadataSP from_context_md = m_master.MaybeGetContextMetadata(m_source_ctx);
    
    if (from_context_md)
    {
        OriginMap &origins = from_context_md->m_origins;
        
        OriginMap::iterator origin_iter = origins.find(from);
        
        if (origin_iter != origins.end())
            to_context_md->m_origins[to] = origin_iter->second;
        
        if (clang::NamespaceDecl *to_namespace = dyn_cast<clang::NamespaceDecl>(to))
        {
            clang::NamespaceDecl *from_namespace = dyn_cast<clang::NamespaceDecl>(from);
            
            NamespaceMetaMap &namespace_maps = from_context_md->m_namespace_maps;
            
            NamespaceMetaMap::iterator namespace_map_iter = namespace_maps.find(from_namespace);
            
            if (namespace_map_iter != namespace_maps.end())
                to_context_md->m_namespace_maps[to_namespace] = namespace_map_iter->second;
        }
    }
    else
    {
        to_context_md->m_origins[to] = DeclOrigin (m_source_ctx, from);
    }
        
    if (TagDecl *from_tag_decl = dyn_cast<TagDecl>(from))
    {
        TagDecl *to_tag_decl = dyn_cast<TagDecl>(to);
        
        to_tag_decl->setHasExternalLexicalStorage();
                        
        if (log)
            log->Printf("    [ClangASTImporter] Imported %p, a %s named %s%s%s [%s->%s]",
                        to,
                        ((clang::Decl*)from_tag_decl)->getDeclKindName(),
                        from_tag_decl->getName().str().c_str(),
                        (to_tag_decl->hasExternalLexicalStorage() ? " Lexical" : ""),
                        (to_tag_decl->hasExternalVisibleStorage() ? " Visible" : ""),
                        (from_tag_decl->isCompleteDefinition() ? "complete" : "incomplete"),
                        (to_tag_decl->isCompleteDefinition() ? "complete" : "incomplete"));
    }
    
    if (isa<NamespaceDecl>(from))
    {
        NamespaceDecl *to_namespace_decl = dyn_cast<NamespaceDecl>(to);
        
        m_master.BuildNamespaceMap(to_namespace_decl);
        
        to_namespace_decl->setHasExternalVisibleStorage();
    }
    
    if (ObjCInterfaceDecl *from_interface_decl = dyn_cast<ObjCInterfaceDecl>(from))
    {
        ObjCInterfaceDecl *to_interface_decl = dyn_cast<ObjCInterfaceDecl>(to);
        
        to_interface_decl->setHasExternalLexicalStorage();
        to_interface_decl->setHasExternalVisibleStorage();
        
        if (to_interface_decl->isForwardDecl())
            to_interface_decl->completedForwardDecl();
        
        to_interface_decl->setExternallyCompleted();
        
        if (log)
            log->Printf("    [ClangASTImporter] Imported %p, a %s named %s%s%s%s",
                        to,
                        ((clang::Decl*)from_interface_decl)->getDeclKindName(),
                        from_interface_decl->getName().str().c_str(),
                        (to_interface_decl->hasExternalLexicalStorage() ? " Lexical" : ""),
                        (to_interface_decl->hasExternalVisibleStorage() ? " Visible" : ""),
                        (to_interface_decl->isForwardDecl() ? " Forward" : ""));
    }
    
    return clang::ASTImporter::Imported(from, to);
}
