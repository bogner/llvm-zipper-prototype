//===-- AppleObjCSymbolVendor.cpp -------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "AppleObjCTypeVendor.h"

#include "lldb/Core/Log.h"
#include "lldb/Core/Module.h"
#include "lldb/Expression/ASTDumper.h"
#include "lldb/Symbol/ClangExternalASTSourceCommon.h"
#include "lldb/Target/ObjCLanguageRuntime.h"
#include "lldb/Target/Process.h"
#include "lldb/Target/Target.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclObjC.h"

using namespace lldb_private;

class lldb_private::AppleObjCExternalASTSource : public ClangExternalASTSourceCommon
{
public:
    AppleObjCExternalASTSource (AppleObjCTypeVendor &type_vendor) :
        m_type_vendor(type_vendor)
    {
    }
    
    clang::DeclContextLookupResult
    FindExternalVisibleDeclsByName (const clang::DeclContext *DC,
                                    clang::DeclarationName Name)
    {
        return clang::DeclContextLookupResult();
    }
    
    clang::ExternalLoadResult
    FindExternalLexicalDecls (const clang::DeclContext *DC,
                              bool (*isKindWeWant)(clang::Decl::Kind),
                              llvm::SmallVectorImpl<clang::Decl*> &Decls)
    {
        return clang::ELR_Success;
    }
    
    void
    CompleteType (clang::TagDecl *Tag)
    {
        return;
    }
    
    void
    CompleteType (clang::ObjCInterfaceDecl *Class)
    {
        return;
    }
    
    bool
    layoutRecordType(const clang::RecordDecl *Record,
                     uint64_t &Size,
                     uint64_t &Alignment,
                     llvm::DenseMap <const clang::FieldDecl *, uint64_t> &FieldOffsets,
                     llvm::DenseMap <const clang::CXXRecordDecl *, clang::CharUnits> &BaseOffsets,
                     llvm::DenseMap <const clang::CXXRecordDecl *, clang::CharUnits> &VirtualBaseOffsets)
    {
        return false;
    }
    
    void StartTranslationUnit (clang::ASTConsumer *Consumer)
    {
        clang::TranslationUnitDecl *translation_unit_decl = m_type_vendor.m_ast_ctx.getASTContext()->getTranslationUnitDecl();
        translation_unit_decl->setHasExternalVisibleStorage();
        translation_unit_decl->setHasExternalLexicalStorage();
    }
private:
    AppleObjCTypeVendor &m_type_vendor;
};

AppleObjCTypeVendor::AppleObjCTypeVendor(ObjCLanguageRuntime &runtime) :
    TypeVendor(),
    m_runtime(runtime),
    m_ast_ctx(runtime.GetProcess()->GetTarget().GetArchitecture().GetTriple().getTriple().c_str())
{
    m_external_source = new AppleObjCExternalASTSource (*this);
    llvm::OwningPtr<clang::ExternalASTSource> external_source_owning_ptr (m_external_source);
    m_ast_ctx.getASTContext()->setExternalSource(external_source_owning_ptr);
}

uint32_t
AppleObjCTypeVendor::FindTypes (const ConstString &name,
                                bool append,
                                uint32_t max_matches,
                                std::vector <ClangASTType> &types)
{
    static unsigned int invocation_id = 0;
    unsigned int current_id = invocation_id++;
    
    lldb::LogSP log(GetLogIfAllCategoriesSet(LIBLLDB_LOG_EXPRESSIONS));  // FIXME - a more appropriate log channel?
    
    if (log)
        log->Printf("AppleObjCTypeVendor::FindTypes [%u] ('%s', %s, %u, )",
                    current_id,
                    (const char*)name.AsCString(),
                    append ? "true" : "false",
                    max_matches);
    
    if (!append)
        types.clear();
    
    uint32_t ret = 0;
    
    do
    {
        // See if the type is already in our ASTContext.
        
        clang::ASTContext *ast_ctx = m_ast_ctx.getASTContext();
        
        clang::IdentifierInfo &identifier_info = ast_ctx->Idents.get(name.GetStringRef());
        clang::DeclarationName decl_name = ast_ctx->DeclarationNames.getIdentifier(&identifier_info);
        
        clang::DeclContext::lookup_const_result lookup_result = ast_ctx->getTranslationUnitDecl()->lookup(decl_name);
        
        if (lookup_result.first != lookup_result.second)
        {
            if (const clang::ObjCInterfaceDecl *result_iface_decl = llvm::dyn_cast<clang::ObjCInterfaceDecl>(*lookup_result.first))
            {
                clang::QualType result_iface_type = ast_ctx->getObjCInterfaceType(result_iface_decl);
                
                if (log)
                {
                    ASTDumper dumper(result_iface_type);
                    log->Printf("AOCTV::FT [%u] Found %s (isa 0x%llx) in the ASTContext",
                                current_id,
                                dumper.GetCString(),
                                m_external_source->GetMetadata((uintptr_t)result_iface_decl));
                }
                    
                types.push_back(ClangASTType(ast_ctx, result_iface_type.getAsOpaquePtr()));
                ret++;
                break;
            }
            else
            {
                if (log)
                    log->Printf("AOCTV::FT [%u] There's something in the ASTContext, but it's not something we know about",
                                current_id);
                break;
            }
        }
        else if(log)
        {
            log->Printf("AOCTV::FT [%u] Couldn't find %s in the ASTContext",
                        current_id,
                        name.AsCString());
        }
        
        // It's not.  If it exists, we have to put it into our ASTContext.
        
        // TODO Actually do this.  But we have to search the class list first.  Until then we'll just give up.
        break;
        
        ObjCLanguageRuntime::ObjCISA isa = m_runtime.GetISA(name);
    
        if (!isa)
        {
            if (log)
                log->Printf("AOCTV::FT [%u] Couldn't find the isa",
                            current_id);
            
            break;
        }
        
        clang::ObjCInterfaceDecl *new_iface_decl = clang::ObjCInterfaceDecl::Create(*ast_ctx,
                                                                                    ast_ctx->getTranslationUnitDecl(),
                                                                                    clang::SourceLocation(),
                                                                                    &identifier_info,
                                                                                    NULL);
        
        m_external_source->SetMetadata((uintptr_t)new_iface_decl, (uint64_t)isa);
        
        new_iface_decl->setHasExternalVisibleStorage();
        
        clang::QualType new_iface_type = ast_ctx->getObjCInterfaceType(new_iface_decl);
        
        if (log)
        {
            ASTDumper dumper(new_iface_type);
            log->Printf("AOCTV::FT [%u] Created %s (isa 0x%llx)",
                        current_id,
                        dumper.GetCString(),
                        (uint64_t)isa);
        }
    } while (0);
    
    return ret;
}
