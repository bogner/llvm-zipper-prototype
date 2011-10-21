//===-- ClangExpressionDeclMap.cpp -----------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "lldb/Expression/ClangExpressionDeclMap.h"

// C Includes
// C++ Includes
// Other libraries and framework includes
// Project includes
#include "clang/AST/DeclarationName.h"
#include "clang/AST/Decl.h"
#include "lldb/lldb-private.h"
#include "lldb/Core/Address.h"
#include "lldb/Core/Error.h"
#include "lldb/Core/Log.h"
#include "lldb/Core/Module.h"
#include "lldb/Core/RegisterValue.h"
#include "lldb/Core/ValueObjectConstResult.h"
#include "lldb/Expression/ASTDumper.h"
#include "lldb/Expression/ClangASTSource.h"
#include "lldb/Expression/ClangPersistentVariables.h"
#include "lldb/Host/Endian.h"
#include "lldb/Symbol/ClangASTContext.h"
#include "lldb/Symbol/ClangNamespaceDecl.h"
#include "lldb/Symbol/CompileUnit.h"
#include "lldb/Symbol/Function.h"
#include "lldb/Symbol/ObjectFile.h"
#include "lldb/Symbol/SymbolContext.h"
#include "lldb/Symbol/SymbolVendor.h"
#include "lldb/Symbol/Type.h"
#include "lldb/Symbol/TypeList.h"
#include "lldb/Symbol/Variable.h"
#include "lldb/Symbol/VariableList.h"
#include "lldb/Target/ExecutionContext.h"
#include "lldb/Target/Process.h"
#include "lldb/Target/RegisterContext.h"
#include "lldb/Target/StackFrame.h"
#include "lldb/Target/Target.h"
#include "lldb/Target/Thread.h"

using namespace lldb;
using namespace lldb_private;
using namespace clang;

ClangExpressionDeclMap::ClangExpressionDeclMap (bool keep_result_in_memory) :
    m_found_entities (),
    m_struct_members (),
    m_keep_result_in_memory (keep_result_in_memory),
    m_parser_vars (),
    m_struct_vars ()
{
    EnableStructVars();
}

ClangExpressionDeclMap::~ClangExpressionDeclMap()
{
    // Note: The model is now that the parser's AST context and all associated
    //   data does not vanish until the expression has been executed.  This means
    //   that valuable lookup data (like namespaces) doesn't vanish, but 
    
    DidParse();
    DidDematerialize();
    DisableStructVars();
}

bool 
ClangExpressionDeclMap::WillParse(ExecutionContext &exe_ctx)
{    
    EnableParserVars();
    m_parser_vars->m_exe_ctx = &exe_ctx;
    
    Target *target = exe_ctx.GetTargetPtr();
    if (exe_ctx.GetFramePtr())
        m_parser_vars->m_sym_ctx = exe_ctx.GetFramePtr()->GetSymbolContext(lldb::eSymbolContextEverything);
    else if (exe_ctx.GetThreadPtr())
        m_parser_vars->m_sym_ctx = exe_ctx.GetThreadPtr()->GetStackFrameAtIndex(0)->GetSymbolContext(lldb::eSymbolContextEverything);
    else if (exe_ctx.GetProcessPtr())
    {
        m_parser_vars->m_sym_ctx.Clear();
        m_parser_vars->m_sym_ctx.target_sp = exe_ctx.GetTargetSP();
    }
    else if (target)
    {
        m_parser_vars->m_sym_ctx.Clear();
        m_parser_vars->m_sym_ctx.target_sp = exe_ctx.GetTargetSP();
    }
    
    if (target)
    {
        m_parser_vars->m_persistent_vars = &target->GetPersistentVariables();
    
        if (!target->GetScratchClangASTContext())
            return false;
    }
    
    m_parser_vars->m_target_info = GetTargetInfo();
    
    return true;
}

void 
ClangExpressionDeclMap::DidParse()
{
    if (m_parser_vars.get())
    {
        for (size_t entity_index = 0, num_entities = m_found_entities.GetSize();
             entity_index < num_entities;
             ++entity_index)
        {
            ClangExpressionVariableSP var_sp(m_found_entities.GetVariableAtIndex(entity_index));
            if (var_sp && 
                var_sp->m_parser_vars.get() && 
                var_sp->m_parser_vars->m_lldb_value)
                delete var_sp->m_parser_vars->m_lldb_value;
            
            var_sp->DisableParserVars();
        }
        
        for (size_t pvar_index = 0, num_pvars = m_parser_vars->m_persistent_vars->GetSize();
             pvar_index < num_pvars;
             ++pvar_index)
        {
            ClangExpressionVariableSP pvar_sp(m_parser_vars->m_persistent_vars->GetVariableAtIndex(pvar_index));
            if (pvar_sp)
                pvar_sp->DisableParserVars();
        }
        
        DisableParserVars();
    }
}

// Interface for IRForTarget

ClangExpressionDeclMap::TargetInfo 
ClangExpressionDeclMap::GetTargetInfo()
{
    assert (m_parser_vars.get());
    
    TargetInfo ret;
    
    ExecutionContext *exe_ctx = m_parser_vars->m_exe_ctx;
    if (exe_ctx)
    {
        Process *process = exe_ctx->GetProcessPtr();
        if (process)
        {
            ret.byte_order = process->GetByteOrder();
            ret.address_byte_size = process->GetAddressByteSize();
        }
        else 
        {
            Target *target = exe_ctx->GetTargetPtr();
            if (target)
            {
                ret.byte_order = target->GetArchitecture().GetByteOrder();
                ret.address_byte_size = target->GetArchitecture().GetAddressByteSize();
            }
        }
    }
    
    return ret;
}

const ConstString &
ClangExpressionDeclMap::GetPersistentResultName ()
{
    assert (m_struct_vars.get());
    assert (m_parser_vars.get());
    if (!m_struct_vars->m_result_name)
    {
        Target *target = m_parser_vars->GetTarget();
        assert (target);
        m_struct_vars->m_result_name = target->GetPersistentVariables().GetNextPersistentVariableName();
    }
    return m_struct_vars->m_result_name;
}

lldb::ClangExpressionVariableSP
ClangExpressionDeclMap::BuildIntegerVariable (const ConstString &name,
                                              lldb_private::TypeFromParser type,
                                              const llvm::APInt& value)
{
    assert (m_parser_vars.get());
    
    ExecutionContext *exe_ctx = m_parser_vars->m_exe_ctx;
    if (exe_ctx == NULL)
        return lldb::ClangExpressionVariableSP();
    Target *target = exe_ctx->GetTargetPtr();

    ASTContext *context(target->GetScratchClangASTContext()->getASTContext());
    
    TypeFromUser user_type(ClangASTContext::CopyType(context, 
                                                     type.GetASTContext(),
                                                     type.GetOpaqueQualType()),
                           context);
    
    if (!m_parser_vars->m_persistent_vars->CreatePersistentVariable (exe_ctx->GetBestExecutionContextScope (),
                                                                     name, 
                                                                     user_type, 
                                                                     m_parser_vars->m_target_info.byte_order,
                                                                     m_parser_vars->m_target_info.address_byte_size))
        return lldb::ClangExpressionVariableSP();
    
    ClangExpressionVariableSP pvar_sp (m_parser_vars->m_persistent_vars->GetVariable(name));
    
    if (!pvar_sp)
        return lldb::ClangExpressionVariableSP();
    
    uint8_t *pvar_data = pvar_sp->GetValueBytes();
    if (pvar_data == NULL)
        return lldb::ClangExpressionVariableSP();
    
    uint64_t value64 = value.getLimitedValue();
        
    size_t num_val_bytes = sizeof(value64);
    size_t num_data_bytes = pvar_sp->GetByteSize();
    
    size_t num_bytes = num_val_bytes;
    if (num_bytes > num_data_bytes)
        num_bytes = num_data_bytes;
    
    for (size_t byte_idx = 0;
         byte_idx < num_bytes;
         ++byte_idx)
    {
        uint64_t shift = byte_idx * 8;
        uint64_t mask = 0xffll << shift;
        uint8_t cur_byte = (uint8_t)((value64 & mask) >> shift);
        
        switch (m_parser_vars->m_target_info.byte_order)
        {
            case eByteOrderBig:
                //                    High         Low
                // Original:         |AABBCCDDEEFFGGHH|
                // Target:                   |EEFFGGHH|
                
                pvar_data[num_data_bytes - (1 + byte_idx)] = cur_byte;
                break;
            case eByteOrderLittle:
                // Target:                   |HHGGFFEE|
                pvar_data[byte_idx] = cur_byte;
                break;
            default:
                return lldb::ClangExpressionVariableSP();    
        }
    }
    
    pvar_sp->m_flags |= ClangExpressionVariable::EVIsFreezeDried;
    pvar_sp->m_flags |= ClangExpressionVariable::EVIsLLDBAllocated;
    pvar_sp->m_flags |= ClangExpressionVariable::EVNeedsAllocation;

    return pvar_sp;
}

lldb::ClangExpressionVariableSP
ClangExpressionDeclMap::BuildCastVariable (const ConstString &name,
                                           VarDecl *decl,
                                           lldb_private::TypeFromParser type)
{
    assert (m_parser_vars.get());
    
    lldb::LogSP log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_EXPRESSIONS));
    
    ExecutionContext *exe_ctx = m_parser_vars->m_exe_ctx;
    if (exe_ctx == NULL)
        return lldb::ClangExpressionVariableSP();
    Target *target = exe_ctx->GetTargetPtr();
    if (target == NULL)
        return lldb::ClangExpressionVariableSP();

    ASTContext *context(target->GetScratchClangASTContext()->getASTContext());
    
    ClangExpressionVariableSP var_sp (m_found_entities.GetVariable(decl));
    
    if (!var_sp)
        var_sp = m_parser_vars->m_persistent_vars->GetVariable(decl);
    
    if (!var_sp)
        return ClangExpressionVariableSP();
    
    TypeFromUser user_type(ClangASTContext::CopyType(context, 
                                                     type.GetASTContext(),
                                                     type.GetOpaqueQualType()),
                           context);
    
    TypeFromUser var_type = var_sp->GetTypeFromUser();
    
    StackFrame *frame = exe_ctx->GetFramePtr();
    if (frame == NULL)
        return lldb::ClangExpressionVariableSP();
    
    VariableSP var = FindVariableInScope (*frame, var_sp->GetName(), &var_type);
    
    if (!var)
        return lldb::ClangExpressionVariableSP(); // but we should handle this; it may be a persistent variable
    
    ValueObjectSP var_valobj = frame->GetValueObjectForFrameVariable(var, lldb::eNoDynamicValues);

    if (!var_valobj)
        return lldb::ClangExpressionVariableSP();
    
    ValueObjectSP var_casted_valobj = var_valobj->CastPointerType(name.GetCString(), user_type);
    
    if (!var_casted_valobj)
        return lldb::ClangExpressionVariableSP();
    
    if (log)
    {
        StreamString my_stream_string;
        
        ClangASTType::DumpTypeDescription (var_type.GetASTContext(),
                                           var_type.GetOpaqueQualType(),
                                           &my_stream_string);
        
        
        log->Printf("Building cast variable to type: %s", my_stream_string.GetString().c_str());
    }
    
    ClangExpressionVariableSP pvar_sp = m_parser_vars->m_persistent_vars->CreatePersistentVariable (var_casted_valobj);
    
    if (!pvar_sp)
        return lldb::ClangExpressionVariableSP();
    
    if (pvar_sp != m_parser_vars->m_persistent_vars->GetVariable(name))
        return lldb::ClangExpressionVariableSP();
    
    pvar_sp->m_flags |= ClangExpressionVariable::EVIsFreezeDried;
    pvar_sp->m_flags |= ClangExpressionVariable::EVIsLLDBAllocated;
    pvar_sp->m_flags |= ClangExpressionVariable::EVNeedsAllocation;
            
    return pvar_sp;
}

bool
ClangExpressionDeclMap::ResultIsReference (const ConstString &name)
{
    ClangExpressionVariableSP pvar_sp = m_parser_vars->m_persistent_vars->GetVariable(name);
    
    return (pvar_sp->m_flags & ClangExpressionVariable::EVIsProgramReference);
}

bool
ClangExpressionDeclMap::CompleteResultVariable (lldb::ClangExpressionVariableSP &valobj, 
                                                lldb_private::Value &value,
                                                const ConstString &name,
                                                lldb_private::TypeFromParser type,
                                                bool transient,
                                                bool maybe_make_load)
{
    assert (m_parser_vars.get());
        
    ClangExpressionVariableSP pvar_sp = m_parser_vars->m_persistent_vars->GetVariable(name);
    
    if (!pvar_sp)
        return false;
        
    if (maybe_make_load && 
        value.GetValueType() == Value::eValueTypeFileAddress &&
        m_parser_vars->m_exe_ctx && 
        m_parser_vars->m_exe_ctx->GetProcessPtr())
    {
        value.SetValueType(Value::eValueTypeLoadAddress);
    }
    
    if (pvar_sp->m_flags & ClangExpressionVariable::EVIsProgramReference &&
        !pvar_sp->m_live_sp &&
        !transient)
    {
        // The reference comes from the program.  We need to set up a live SP for it.
        
        pvar_sp->m_live_sp = ValueObjectConstResult::Create(m_parser_vars->m_exe_ctx->GetBestExecutionContextScope(),
                                                            pvar_sp->GetTypeFromUser().GetASTContext(),
                                                            pvar_sp->GetTypeFromUser().GetOpaqueQualType(),
                                                            pvar_sp->GetName(),
                                                            value.GetScalar().ULongLong(),
                                                            value.GetValueAddressType(),
                                                            pvar_sp->GetByteSize());
    }
    
    if (pvar_sp->m_flags & ClangExpressionVariable::EVNeedsFreezeDry)
    {
        pvar_sp->ValueUpdated();
        
        const size_t pvar_byte_size = pvar_sp->GetByteSize();
        uint8_t *pvar_data = pvar_sp->GetValueBytes();
        
        if (!ReadTarget(pvar_data, value, pvar_byte_size))
            return false;
        
        pvar_sp->m_flags &= ~(ClangExpressionVariable::EVNeedsFreezeDry);
    }
    
    valobj = pvar_sp;
    
    return true;
}

bool 
ClangExpressionDeclMap::AddPersistentVariable 
(
    const NamedDecl *decl, 
    const ConstString &name, 
    TypeFromParser parser_type,
    bool is_result,
    bool is_lvalue
)
{
    assert (m_parser_vars.get());
    
    lldb::LogSP log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_EXPRESSIONS));
    ExecutionContext *exe_ctx = m_parser_vars->m_exe_ctx;
    if (exe_ctx == NULL)
        return false;
    Target *target = exe_ctx->GetTargetPtr();
    if (target == NULL)
        return false;

    ASTContext *context(target->GetScratchClangASTContext()->getASTContext());
    
    TypeFromUser user_type(ClangASTContext::CopyType(context, 
                                                     parser_type.GetASTContext(),
                                                     parser_type.GetOpaqueQualType()),
                           context);
        
    if (!m_parser_vars->m_target_info.IsValid())
        return false;
    
    if (!m_parser_vars->m_persistent_vars->CreatePersistentVariable (exe_ctx->GetBestExecutionContextScope (),
                                                                     name, 
                                                                     user_type, 
                                                                     m_parser_vars->m_target_info.byte_order,
                                                                     m_parser_vars->m_target_info.address_byte_size))
        return false;
    
    ClangExpressionVariableSP var_sp (m_parser_vars->m_persistent_vars->GetVariable(name));
    
    if (!var_sp)
        return false;
    
    if (is_result)
        var_sp->m_flags |= ClangExpressionVariable::EVNeedsFreezeDry;
    else
        var_sp->m_flags |= ClangExpressionVariable::EVKeepInTarget; // explicitly-declared persistent variables should persist
    
    if (is_lvalue)
    {
        var_sp->m_flags |= ClangExpressionVariable::EVIsProgramReference;
    }
    else
    {
        var_sp->m_flags |= ClangExpressionVariable::EVIsLLDBAllocated;
        var_sp->m_flags |= ClangExpressionVariable::EVNeedsAllocation;
    }
    
    if (log)
        log->Printf("Created persistent variable with flags 0x%hx", var_sp->m_flags);
    
    var_sp->EnableParserVars();
    
    var_sp->m_parser_vars->m_named_decl = decl;
    var_sp->m_parser_vars->m_parser_type = parser_type;
    
    return true;
}

bool 
ClangExpressionDeclMap::AddValueToStruct 
(
    const NamedDecl *decl,
    const ConstString &name,
    llvm::Value *value,
    size_t size,
    off_t alignment
)
{
    assert (m_struct_vars.get());
    assert (m_parser_vars.get());
    
    lldb::LogSP log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_EXPRESSIONS));
    
    m_struct_vars->m_struct_laid_out = false;
    
    if (m_struct_members.GetVariable(decl))
        return true;
    
    ClangExpressionVariableSP var_sp (m_found_entities.GetVariable(decl));
    
    if (!var_sp)
        var_sp = m_parser_vars->m_persistent_vars->GetVariable(decl);
    
    if (!var_sp)
        return false;
    
    if (log)
        log->Printf("Adding value for decl %p [%s - %s] to the structure",
                    decl,
                    name.GetCString(),
                    var_sp->GetName().GetCString());
    
    // We know entity->m_parser_vars is valid because we used a parser variable
    // to find it
    var_sp->m_parser_vars->m_llvm_value = value;
    
    var_sp->EnableJITVars();
    var_sp->m_jit_vars->m_alignment = alignment;
    var_sp->m_jit_vars->m_size = size;
    
    m_struct_members.AddVariable(var_sp);
    
    return true;
}

bool
ClangExpressionDeclMap::DoStructLayout ()
{
    assert (m_struct_vars.get());
    
    if (m_struct_vars->m_struct_laid_out)
        return true;
    
    off_t cursor = 0;
    
    m_struct_vars->m_struct_alignment = 0;
    m_struct_vars->m_struct_size = 0;
    
    for (size_t member_index = 0, num_members = m_struct_members.GetSize();
         member_index < num_members;
         ++member_index)
    {
        ClangExpressionVariableSP member_sp(m_struct_members.GetVariableAtIndex(member_index));
        if (!member_sp)
            return false;

        if (!member_sp->m_jit_vars.get())
            return false;
        
        if (member_index == 0)
            m_struct_vars->m_struct_alignment = member_sp->m_jit_vars->m_alignment;
        
        if (cursor % member_sp->m_jit_vars->m_alignment)
            cursor += (member_sp->m_jit_vars->m_alignment - (cursor % member_sp->m_jit_vars->m_alignment));
        
        member_sp->m_jit_vars->m_offset = cursor;
        cursor += member_sp->m_jit_vars->m_size;
    }
    
    m_struct_vars->m_struct_size = cursor;
    
    m_struct_vars->m_struct_laid_out = true;
    return true;
}

bool ClangExpressionDeclMap::GetStructInfo 
(
    uint32_t &num_elements,
    size_t &size,
    off_t &alignment
)
{
    assert (m_struct_vars.get());
    
    if (!m_struct_vars->m_struct_laid_out)
        return false;
    
    num_elements = m_struct_members.GetSize();
    size = m_struct_vars->m_struct_size;
    alignment = m_struct_vars->m_struct_alignment;
    
    return true;
}

bool 
ClangExpressionDeclMap::GetStructElement 
(
    const NamedDecl *&decl,
    llvm::Value *&value,
    off_t &offset,
    ConstString &name,
    uint32_t index
)
{
    assert (m_struct_vars.get());
    
    if (!m_struct_vars->m_struct_laid_out)
        return false;
    
    if (index >= m_struct_members.GetSize())
        return false;
    
    ClangExpressionVariableSP member_sp(m_struct_members.GetVariableAtIndex(index));
    
    if (!member_sp ||
        !member_sp->m_parser_vars.get() ||
        !member_sp->m_jit_vars.get())
        return false;
    
    decl = member_sp->m_parser_vars->m_named_decl;
    value = member_sp->m_parser_vars->m_llvm_value;
    offset = member_sp->m_jit_vars->m_offset;
    name = member_sp->GetName();
        
    return true;
}

bool
ClangExpressionDeclMap::GetFunctionInfo 
(
    const NamedDecl *decl, 
    llvm::Value**& value, 
    uint64_t &ptr
)
{
    ClangExpressionVariableSP entity_sp(m_found_entities.GetVariable(decl));

    if (!entity_sp)
        return false;
    
    // We know m_parser_vars is valid since we searched for the variable by
    // its NamedDecl
    
    value = &entity_sp->m_parser_vars->m_llvm_value;
    ptr = entity_sp->m_parser_vars->m_lldb_value->GetScalar().ULongLong();
    
    return true;
}

static void
FindCodeSymbolInContext
(
    const ConstString &name,
    SymbolContext &sym_ctx,
    SymbolContextList &sc_list
)
{
    if (sym_ctx.module_sp)
       sym_ctx.module_sp->FindSymbolsWithNameAndType(name, eSymbolTypeCode, sc_list);
    
    if (!sc_list.GetSize())
        sym_ctx.target_sp->GetImages().FindSymbolsWithNameAndType(name, eSymbolTypeCode, sc_list);
}

bool
ClangExpressionDeclMap::GetFunctionAddress 
(
    const ConstString &name,
    uint64_t &func_addr
)
{
    assert (m_parser_vars.get());
    
    lldb::LogSP log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_EXPRESSIONS));
    ExecutionContext *exe_ctx = m_parser_vars->m_exe_ctx;
    if (exe_ctx == NULL)
        return false;
    Target *target = exe_ctx->GetTargetPtr();
    // Back out in all cases where we're not fully initialized
    if (target == NULL)
        return false;
    if (!m_parser_vars->m_sym_ctx.target_sp)
        return false;

    SymbolContextList sc_list;
    
    FindCodeSymbolInContext(name, m_parser_vars->m_sym_ctx, sc_list);
        
    if (!sc_list.GetSize())
    {
        // We occasionally get debug information in which a const function is reported 
        // as non-const, so the mangled name is wrong.  This is a hack to compensate.
        
        Mangled mangled(name.GetCString(), true);
        
        ConstString demangled_name = mangled.GetDemangledName();
        
        if (strlen(demangled_name.GetCString()))
        {
            std::string const_name_scratch(demangled_name.GetCString());
            
            const_name_scratch.append(" const");
            
            ConstString const_name(const_name_scratch.c_str());
                        
            FindCodeSymbolInContext(name, m_parser_vars->m_sym_ctx, sc_list);
            
            if (log)
                log->Printf("Found %d results with const name %s", sc_list.GetSize(), const_name.GetCString());
        }
    }
    
    if (!sc_list.GetSize())
        return false;
    
    SymbolContext sym_ctx;
    sc_list.GetContextAtIndex(0, sym_ctx);
    
    const Address *func_so_addr = NULL;
    
    if (sym_ctx.function)
        func_so_addr = &sym_ctx.function->GetAddressRange().GetBaseAddress();
    else if (sym_ctx.symbol)
        func_so_addr = &sym_ctx.symbol->GetAddressRangeRef().GetBaseAddress();
    else
        return false;
    
    if (!func_so_addr || !func_so_addr->IsValid())
        return false;
    
    func_addr = func_so_addr->GetCallableLoadAddress (target);

    return true;
}

addr_t
ClangExpressionDeclMap::GetSymbolAddress (Target &target, const ConstString &name)
{
    SymbolContextList sc_list;
    
    target.GetImages().FindSymbolsWithNameAndType(name, eSymbolTypeAny, sc_list);
    
    const uint32_t num_matches = sc_list.GetSize();
    addr_t symbol_load_addr = LLDB_INVALID_ADDRESS;

    for (uint32_t i=0; i<num_matches && symbol_load_addr == LLDB_INVALID_ADDRESS; i++)
    {
        SymbolContext sym_ctx;
        sc_list.GetContextAtIndex(i, sym_ctx);
    
        const Address *sym_address = &sym_ctx.symbol->GetAddressRangeRef().GetBaseAddress();
        
        if (!sym_address || !sym_address->IsValid())
            return LLDB_INVALID_ADDRESS;
        
        if (sym_address)
        {
            switch (sym_ctx.symbol->GetType())
            {
                case eSymbolTypeCode:
                case eSymbolTypeTrampoline:
                    symbol_load_addr = sym_address->GetCallableLoadAddress (&target);
                    break;
                    
                case eSymbolTypeData:
                case eSymbolTypeRuntime:
                case eSymbolTypeVariable:
                case eSymbolTypeLocal:
                case eSymbolTypeParam:
                case eSymbolTypeInvalid:
                case eSymbolTypeAbsolute:
                case eSymbolTypeExtern:
                case eSymbolTypeException:
                case eSymbolTypeSourceFile:
                case eSymbolTypeHeaderFile:
                case eSymbolTypeObjectFile:
                case eSymbolTypeCommonBlock:
                case eSymbolTypeBlock:
                case eSymbolTypeVariableType:
                case eSymbolTypeLineEntry:
                case eSymbolTypeLineHeader:
                case eSymbolTypeScopeBegin:
                case eSymbolTypeScopeEnd:
                case eSymbolTypeAdditional:
                case eSymbolTypeCompiler:
                case eSymbolTypeInstrumentation:
                case eSymbolTypeUndefined:
                    symbol_load_addr = sym_address->GetLoadAddress (&target);
                    break;
            }
        }
    }
    
    return symbol_load_addr;
}

addr_t
ClangExpressionDeclMap::GetSymbolAddress (const ConstString &name)
{
    assert (m_parser_vars.get());
    
    if (!m_parser_vars->m_exe_ctx ||
        !m_parser_vars->m_exe_ctx->GetTargetPtr())
        return false;
    
    return GetSymbolAddress(m_parser_vars->m_exe_ctx->GetTargetRef(), name);
}

// Interface for IRInterpreter

Value 
ClangExpressionDeclMap::WrapBareAddress (lldb::addr_t addr)
{
    Value ret;

    ret.SetContext(Value::eContextTypeInvalid, NULL);

    if (m_parser_vars->m_exe_ctx && m_parser_vars->m_exe_ctx->GetProcessPtr())
        ret.SetValueType(Value::eValueTypeLoadAddress);
    else
        ret.SetValueType(Value::eValueTypeFileAddress);

    ret.GetScalar() = (unsigned long long)addr;

    return ret;
}

bool
ClangExpressionDeclMap::WriteTarget (lldb_private::Value &value,
                                     const uint8_t *data,
                                     size_t length)
{
    assert (m_parser_vars.get());
    
    ExecutionContext *exe_ctx = m_parser_vars->m_exe_ctx;
    
    Process *process = exe_ctx->GetProcessPtr();
    if (value.GetContextType() == Value::eContextTypeRegisterInfo)
    {
        if (!process)
            return false;
        
        RegisterContext *reg_ctx = exe_ctx->GetRegisterContext();
        RegisterInfo *reg_info = value.GetRegisterInfo();
        
        if (!reg_ctx)
            return false;
        
        lldb_private::RegisterValue reg_value;
        Error err;
        
        if (!reg_value.SetFromMemoryData (reg_info, data, length, process->GetByteOrder(), err))
            return false;
        
        return reg_ctx->WriteRegister(reg_info, reg_value);
    }
    else
    {
        switch (value.GetValueType())
        {
        default:
            return false;
        case Value::eValueTypeFileAddress:
            {
                if (!process)
                    return false;
                
                Target *target = exe_ctx->GetTargetPtr();
                Address file_addr;
                
                if (!target->GetImages().ResolveFileAddress((lldb::addr_t)value.GetScalar().ULongLong(), file_addr))
                    return false;
                
                lldb::addr_t load_addr = file_addr.GetLoadAddress(target);
                
                Error err;
                process->WriteMemory(load_addr, data, length, err);
                
                return err.Success();
            }
        case Value::eValueTypeLoadAddress:
            {
                if (!process)
                    return false;
                
                Error err;
                process->WriteMemory((lldb::addr_t)value.GetScalar().ULongLong(), data, length, err);
    
                return err.Success();
            }
        case Value::eValueTypeHostAddress:
            memcpy ((void *)value.GetScalar().ULongLong(), data, length);
            return true;
        case Value::eValueTypeScalar:
            return false;
        }
    }
}

bool
ClangExpressionDeclMap::ReadTarget (uint8_t *data,
                                    lldb_private::Value &value,
                                    size_t length)
{
    assert (m_parser_vars.get());
    
    ExecutionContext *exe_ctx = m_parser_vars->m_exe_ctx;

    Process *process = exe_ctx->GetProcessPtr();

    if (value.GetContextType() == Value::eContextTypeRegisterInfo)
    {
        if (!process)
            return false;
        
        RegisterContext *reg_ctx = exe_ctx->GetRegisterContext();
        RegisterInfo *reg_info = value.GetRegisterInfo();
        
        if (!reg_ctx)
            return false;
        
        lldb_private::RegisterValue reg_value;
        Error err;
        
        if (!reg_ctx->ReadRegister(reg_info, reg_value))
            return false;
        
        return reg_value.GetAsMemoryData(reg_info, data, length, process->GetByteOrder(), err);        
    }
    else
    {
        switch (value.GetValueType())
        {
            default:
                return false;
            case Value::eValueTypeFileAddress:
            {
                Target *target = exe_ctx->GetTargetPtr();
                if (target == NULL)
                    return false;
                
                Address file_addr;
                
                if (!target->GetImages().ResolveFileAddress((lldb::addr_t)value.GetScalar().ULongLong(), file_addr))
                    return false;
                
                Error err;
                target->ReadMemory(file_addr, true, data, length, err);
                
                return err.Success();
            }
            case Value::eValueTypeLoadAddress:
            {
                if (!process)
                    return false;
                
                Error err;
                process->ReadMemory((lldb::addr_t)value.GetScalar().ULongLong(), data, length, err);
                
                return err.Success();
            }
            case Value::eValueTypeHostAddress:
                memcpy (data, (const void *)value.GetScalar().ULongLong(), length);
                return true;
            case Value::eValueTypeScalar:
                return false;
        }
    }
}

lldb_private::Value
ClangExpressionDeclMap::LookupDecl (clang::NamedDecl *decl)
{
    assert (m_parser_vars.get());
    
    ExecutionContext exe_ctx = *m_parser_vars->m_exe_ctx;
        
    ClangExpressionVariableSP expr_var_sp (m_found_entities.GetVariable(decl));
    ClangExpressionVariableSP persistent_var_sp (m_parser_vars->m_persistent_vars->GetVariable(decl));

    if (expr_var_sp)
    {
        if (!expr_var_sp->m_parser_vars.get() || !expr_var_sp->m_parser_vars->m_lldb_var)
            return Value();
        
        return *GetVariableValue(exe_ctx, expr_var_sp->m_parser_vars->m_lldb_var, NULL);
    }
    else if (persistent_var_sp)
    {
        if ((persistent_var_sp->m_flags & ClangExpressionVariable::EVIsProgramReference ||
             persistent_var_sp->m_flags & ClangExpressionVariable::EVIsLLDBAllocated) &&
            persistent_var_sp->m_live_sp)
        {
            return persistent_var_sp->m_live_sp->GetValue();
        }
        else
        {
            lldb_private::Value ret;
            ret.SetValueType(Value::eValueTypeHostAddress);
            ret.SetContext(Value::eContextTypeInvalid, NULL);
            ret.GetScalar() = (lldb::addr_t)persistent_var_sp->GetValueBytes();
            return ret;
        }
    }
    else
    {
        return Value();
    }
}

// Interface for CommandObjectExpression

bool 
ClangExpressionDeclMap::Materialize 
(
    ExecutionContext &exe_ctx, 
    lldb::addr_t &struct_address,
    Error &err
)
{
    EnableMaterialVars();
    
    m_material_vars->m_process = exe_ctx.GetProcessPtr();
    
    bool result = DoMaterialize(false /* dematerialize */, 
                                exe_ctx, 
                                LLDB_INVALID_ADDRESS /* top of stack frame */, 
                                LLDB_INVALID_ADDRESS /* bottom of stack frame */, 
                                NULL, /* result SP */
                                err);
    
    if (result)
        struct_address = m_material_vars->m_materialized_location;
    
    return result;
}

bool 
ClangExpressionDeclMap::GetObjectPointer
(
    lldb::addr_t &object_ptr,
    ConstString &object_name,
    ExecutionContext &exe_ctx,
    Error &err,
    bool suppress_type_check
)
{
    assert (m_struct_vars.get());
    
    Target *target = exe_ctx.GetTargetPtr();
    Process *process = exe_ctx.GetProcessPtr();
    StackFrame *frame = exe_ctx.GetFramePtr();

    if (frame == NULL || process == NULL || target == NULL)
    {
        err.SetErrorString("Couldn't load 'this' because the context is incomplete");
        return false;
    }
    
    if (!m_struct_vars->m_object_pointer_type.GetOpaqueQualType())
    {
        err.SetErrorString("Couldn't load 'this' because its type is unknown");
        return false;
    }
    
    VariableSP object_ptr_var = FindVariableInScope (*frame,
                                                     object_name, 
                                                     (suppress_type_check ? NULL : &m_struct_vars->m_object_pointer_type));
    
    if (!object_ptr_var)
    {
        err.SetErrorStringWithFormat("Couldn't find '%s' with appropriate type in scope", object_name.GetCString());
        return false;
    }
    
    std::auto_ptr<lldb_private::Value> location_value(GetVariableValue(exe_ctx,
                                                                       object_ptr_var,
                                                                       NULL));
    
    if (!location_value.get())
    {
        err.SetErrorStringWithFormat("Couldn't get the location for '%s'", object_name.GetCString());
        return false;
    }
    
    switch (location_value->GetValueType())
    {
    default:
        err.SetErrorStringWithFormat("'%s' is not in memory; LLDB must be extended to handle registers", object_name.GetCString());
        return false;
    case Value::eValueTypeLoadAddress:
        {
            lldb::addr_t value_addr = location_value->GetScalar().ULongLong();
            uint32_t address_byte_size = target->GetArchitecture().GetAddressByteSize();
            
            if (ClangASTType::GetClangTypeBitWidth(m_struct_vars->m_object_pointer_type.GetASTContext(), 
                                                   m_struct_vars->m_object_pointer_type.GetOpaqueQualType()) != address_byte_size * 8)
            {
                err.SetErrorStringWithFormat("'%s' is not of an expected pointer size", object_name.GetCString());
                return false;
            }
            
            Error read_error;
            object_ptr = process->ReadPointerFromMemory (value_addr, read_error);
            if (read_error.Fail() || object_ptr == LLDB_INVALID_ADDRESS)
            {
                err.SetErrorStringWithFormat("Coldn't read '%s' from the target: %s", object_name.GetCString(), read_error.AsCString());
                return false;
            }            
            return true;
        }
    case Value::eValueTypeScalar:
        {
            if (location_value->GetContextType() != Value::eContextTypeRegisterInfo)
            {
                StreamString ss;
                location_value->Dump(&ss);
                
                err.SetErrorStringWithFormat("%s is a scalar of unhandled type: %s", object_name.GetCString(), ss.GetString().c_str());
                return false;
            }
                        
            RegisterInfo *reg_info = location_value->GetRegisterInfo();
            
            if (!reg_info)
            {
                err.SetErrorStringWithFormat("Couldn't get the register information for %s", object_name.GetCString());
                return false;
            }
            
            RegisterContext *reg_ctx = exe_ctx.GetRegisterContext();
            
            if (!reg_ctx)
            {
                err.SetErrorStringWithFormat("Couldn't read register context to read %s from %s", object_name.GetCString(), reg_info->name);
                return false;
            }
            
            uint32_t register_number = reg_info->kinds[lldb::eRegisterKindLLDB];
            
            object_ptr = reg_ctx->ReadRegisterAsUnsigned(register_number, 0x0);
            
            return true;
        }
    }
}

bool 
ClangExpressionDeclMap::Dematerialize 
(
    ExecutionContext &exe_ctx,
    ClangExpressionVariableSP &result_sp,
    lldb::addr_t stack_frame_top,
    lldb::addr_t stack_frame_bottom,
    Error &err
)
{
    return DoMaterialize(true, exe_ctx, stack_frame_top, stack_frame_bottom, &result_sp, err);
    
    DidDematerialize();
}

void
ClangExpressionDeclMap::DidDematerialize()
{
    if (m_material_vars.get())
    {
        if (m_material_vars->m_materialized_location)
        {        
            //#define SINGLE_STEP_EXPRESSIONS
            
#ifndef SINGLE_STEP_EXPRESSIONS
            m_material_vars->m_process->DeallocateMemory(m_material_vars->m_materialized_location);
#endif
            m_material_vars->m_materialized_location = 0;
        }
        
        DisableMaterialVars();
    }
}

bool
ClangExpressionDeclMap::DumpMaterializedStruct
(
    ExecutionContext &exe_ctx, 
    Stream &s,
    Error &err
)
{
    assert (m_struct_vars.get());
    assert (m_material_vars.get());
    
    if (!m_struct_vars->m_struct_laid_out)
    {
        err.SetErrorString("Structure hasn't been laid out yet");
        return false;
    }
    Process *process = exe_ctx.GetProcessPtr();

    if (!process)
    {
        err.SetErrorString("Couldn't find the process");
        return false;
    }
    
    Target *target = exe_ctx.GetTargetPtr();
    if (!target)
    {
        err.SetErrorString("Couldn't find the target");
        return false;
    }
    
    if (!m_material_vars->m_materialized_location)
    {
        err.SetErrorString("No materialized location");
        return false;
    }
    
    lldb::DataBufferSP data_sp(new DataBufferHeap(m_struct_vars->m_struct_size, 0));    
    
    Error error;
    if (process->ReadMemory (m_material_vars->m_materialized_location, 
                                     data_sp->GetBytes(), 
                                     data_sp->GetByteSize(), error) != data_sp->GetByteSize())
    {
        err.SetErrorStringWithFormat ("Couldn't read struct from the target: %s", error.AsCString());
        return false;
    }
    
    DataExtractor extractor(data_sp, process->GetByteOrder(), target->GetArchitecture().GetAddressByteSize());
    
    for (size_t member_idx = 0, num_members = m_struct_members.GetSize();
         member_idx < num_members;
         ++member_idx)
    {
        ClangExpressionVariableSP member_sp(m_struct_members.GetVariableAtIndex(member_idx));
        
        if (!member_sp)
            return false;

        s.Printf("[%s]\n", member_sp->GetName().GetCString());
        
        if (!member_sp->m_jit_vars.get())
            return false;
        
        extractor.Dump (&s,                                                                          // stream
                        member_sp->m_jit_vars->m_offset,                                             // offset
                        lldb::eFormatBytesWithASCII,                                                 // format
                        1,                                                                           // byte size of individual entries
                        member_sp->m_jit_vars->m_size,                                               // number of entries
                        16,                                                                          // entries per line
                        m_material_vars->m_materialized_location + member_sp->m_jit_vars->m_offset,  // address to print
                        0,                                                                           // bit size (bitfields only; 0 means ignore)
                        0);                                                                          // bit alignment (bitfields only; 0 means ignore)
        
        s.PutChar('\n');
    }
    
    return true;
}

bool 
ClangExpressionDeclMap::DoMaterialize 
(
    bool dematerialize,
    ExecutionContext &exe_ctx,
    lldb::addr_t stack_frame_top,
    lldb::addr_t stack_frame_bottom,
    lldb::ClangExpressionVariableSP *result_sp_ptr,
    Error &err
)
{
    if (result_sp_ptr)
        result_sp_ptr->reset();

    assert (m_struct_vars.get());
    
    lldb::LogSP log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_EXPRESSIONS));
    
    if (!m_struct_vars->m_struct_laid_out)
    {
        err.SetErrorString("Structure hasn't been laid out yet");
        return false;
    }
    
    StackFrame *frame = exe_ctx.GetFramePtr();
    if (!frame)
    {
        err.SetErrorString("Received null execution frame");
        return false;
    }
    Target *target = exe_ctx.GetTargetPtr();
    
    ClangPersistentVariables &persistent_vars = target->GetPersistentVariables();
        
    if (!m_struct_vars->m_struct_size)
    {
        if (log)
            log->PutCString("Not bothering to allocate a struct because no arguments are needed");
        
        m_material_vars->m_allocated_area = NULL;
        
        return true;
    }
    
    const SymbolContext &sym_ctx(frame->GetSymbolContext(lldb::eSymbolContextEverything));
    
    if (!dematerialize)
    {
        Process *process = exe_ctx.GetProcessPtr();
        if (m_material_vars->m_materialized_location)
        {
            process->DeallocateMemory(m_material_vars->m_materialized_location);
            m_material_vars->m_materialized_location = 0;
        }
        
        if (log)
            log->PutCString("Allocating memory for materialized argument struct");
        
        lldb::addr_t mem = process->AllocateMemory(m_struct_vars->m_struct_alignment + m_struct_vars->m_struct_size, 
                                                           lldb::ePermissionsReadable | lldb::ePermissionsWritable,
                                                           err);
        
        if (mem == LLDB_INVALID_ADDRESS)
            return false;
        
        m_material_vars->m_allocated_area = mem;
    }
    
    m_material_vars->m_materialized_location = m_material_vars->m_allocated_area;
    
    if (m_material_vars->m_materialized_location % m_struct_vars->m_struct_alignment)
        m_material_vars->m_materialized_location += (m_struct_vars->m_struct_alignment - (m_material_vars->m_materialized_location % m_struct_vars->m_struct_alignment));
    
    for (uint64_t member_index = 0, num_members = m_struct_members.GetSize();
         member_index < num_members;
         ++member_index)
    {
        ClangExpressionVariableSP member_sp(m_struct_members.GetVariableAtIndex(member_index));
        
        if (m_found_entities.ContainsVariable (member_sp))
        {
            RegisterInfo *reg_info = member_sp->GetRegisterInfo ();
            if (reg_info)
            {
                // This is a register variable
                
                RegisterContext *reg_ctx = exe_ctx.GetRegisterContext();
                
                if (!reg_ctx)
                    return false;
                
                if (!DoMaterializeOneRegister (dematerialize, 
                                               exe_ctx, 
                                               *reg_ctx, 
                                               *reg_info, 
                                               m_material_vars->m_materialized_location + member_sp->m_jit_vars->m_offset, 
                                               err))
                    return false;
            }
            else
            {
                if (!member_sp->m_jit_vars.get())
                    return false;
                
                if (!DoMaterializeOneVariable (dematerialize, 
                                               exe_ctx, 
                                               sym_ctx,
                                               member_sp,
                                               m_material_vars->m_materialized_location + member_sp->m_jit_vars->m_offset, 
                                               err))
                    return false;
            }
        }
        else
        {
            // No need to look for presistent variables if the name doesn't start 
            // with with a '$' character...
            if (member_sp->GetName().AsCString ("!")[0] == '$' && persistent_vars.ContainsVariable(member_sp))
            {
                
                if (member_sp->GetName() == m_struct_vars->m_result_name)
                {
                    if (log)
                        log->PutCString("Found result member in the struct");

                    if (result_sp_ptr)
                        *result_sp_ptr = member_sp;
                    
                }

                if (!DoMaterializeOnePersistentVariable (dematerialize, 
                                                         exe_ctx,
                                                         member_sp, 
                                                         m_material_vars->m_materialized_location + member_sp->m_jit_vars->m_offset,
                                                         stack_frame_top,
                                                         stack_frame_bottom,
                                                         err))
                    return false;
            }
            else
            {
                err.SetErrorStringWithFormat("Unexpected variable %s", member_sp->GetName().GetCString());
                return false;
            }
        }
    }
    
    return true;
}

bool
ClangExpressionDeclMap::DoMaterializeOnePersistentVariable
(
    bool dematerialize,
    ExecutionContext &exe_ctx,
    ClangExpressionVariableSP &var_sp,
    lldb::addr_t addr,
    lldb::addr_t stack_frame_top,
    lldb::addr_t stack_frame_bottom,
    Error &err
)
{
    lldb::LogSP log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_EXPRESSIONS));

    if (!var_sp)
    {
        err.SetErrorString("Invalid persistent variable");
        return LLDB_INVALID_ADDRESS;
    }
    
    const size_t pvar_byte_size = var_sp->GetByteSize();
    
    uint8_t *pvar_data = var_sp->GetValueBytes();
    if (pvar_data == NULL)
        return false;
    
    Error error;
    Process *process = exe_ctx.GetProcessPtr();

    lldb::addr_t mem; // The address of a spare memory area used to hold the persistent variable.
    
    if (dematerialize)
    {
        if (log)
            log->Printf("Dematerializing persistent variable with flags 0x%hx", var_sp->m_flags);
        
        if ((var_sp->m_flags & ClangExpressionVariable::EVIsLLDBAllocated) ||
            (var_sp->m_flags & ClangExpressionVariable::EVIsProgramReference))
        {
            // Get the location of the target out of the struct.
            
            Error read_error;
            mem = process->ReadPointerFromMemory (addr, read_error);
            
            if (mem == LLDB_INVALID_ADDRESS)
            {
                err.SetErrorStringWithFormat("Couldn't read address of %s from struct: %s", var_sp->GetName().GetCString(), error.AsCString());
                return false;
            }
            
            if (var_sp->m_flags & ClangExpressionVariable::EVIsProgramReference &&
                !var_sp->m_live_sp)
            {
                // If the reference comes from the program, then the ClangExpressionVariable's
                // live variable data hasn't been set up yet.  Do this now.
                
                var_sp->m_live_sp = ValueObjectConstResult::Create (exe_ctx.GetBestExecutionContextScope (),
                                                                    var_sp->GetTypeFromUser().GetASTContext(),
                                                                    var_sp->GetTypeFromUser().GetOpaqueQualType(),
                                                                    var_sp->GetName(),
                                                                    mem,
                                                                    eAddressTypeLoad,
                                                                    pvar_byte_size);
            }
            
            if (!var_sp->m_live_sp)
            {
                err.SetErrorStringWithFormat("Couldn't find the memory area used to store %s", var_sp->GetName().GetCString());
                return false;
            }
            
            if (var_sp->m_live_sp->GetValue().GetValueAddressType() != eAddressTypeLoad)
            {
                err.SetErrorStringWithFormat("The address of the memory area for %s is in an incorrect format", var_sp->GetName().GetCString());
                return false;
            }
            
            if (var_sp->m_flags & ClangExpressionVariable::EVNeedsFreezeDry ||
                var_sp->m_flags & ClangExpressionVariable::EVKeepInTarget)
            {
                mem = var_sp->m_live_sp->GetValue().GetScalar().ULongLong();
                
                if (log)
                    log->Printf("Dematerializing %s from 0x%llx", var_sp->GetName().GetCString(), (uint64_t)mem);
                
                // Read the contents of the spare memory area
                                
                var_sp->ValueUpdated ();
                if (process->ReadMemory (mem, pvar_data, pvar_byte_size, error) != pvar_byte_size)
                {
                    err.SetErrorStringWithFormat ("Couldn't read a composite type from the target: %s", error.AsCString());
                    return false;
                }
                
                if (stack_frame_top != LLDB_INVALID_ADDRESS &&
                    stack_frame_bottom != LLDB_INVALID_ADDRESS &&
                    mem >= stack_frame_bottom &&
                    mem <= stack_frame_top)
                {
                    // If the variable is resident in the stack frame created by the expression,
                    // then it cannot be relied upon to stay around.  We treat it as needing
                    // reallocation.
                    
                    var_sp->m_flags |= ClangExpressionVariable::EVIsLLDBAllocated;
                    var_sp->m_flags |= ClangExpressionVariable::EVNeedsAllocation;
                    var_sp->m_flags &= ~ClangExpressionVariable::EVIsProgramReference;
                }
                
                var_sp->m_flags &= ~ClangExpressionVariable::EVNeedsFreezeDry;
            }
            
            if (var_sp->m_flags & ClangExpressionVariable::EVNeedsAllocation &&
                !(var_sp->m_flags & ClangExpressionVariable::EVKeepInTarget))
            {
                if (m_keep_result_in_memory)
                {
                    var_sp->m_flags |= ClangExpressionVariable::EVKeepInTarget;
                }
                else
                {
                    Error deallocate_error = process->DeallocateMemory(mem);
                    
                    if (!err.Success())
                    {
                        err.SetErrorStringWithFormat ("Couldn't deallocate memory for %s: %s", var_sp->GetName().GetCString(), deallocate_error.AsCString());
                        return false;
                    }
                }
            }
        }
        else
        {
            err.SetErrorStringWithFormat("Persistent variables without separate allocations are not currently supported.");
            return false;
        }
    }
    else 
    {
        if (log)
            log->Printf("Materializing persistent variable with flags 0x%hx", var_sp->m_flags);
        
        if (var_sp->m_flags & ClangExpressionVariable::EVNeedsAllocation)
        {
            // Allocate a spare memory area to store the persistent variable's contents.
            
            Error allocate_error;
            
            mem = process->AllocateMemory(pvar_byte_size, 
                                                  lldb::ePermissionsReadable | lldb::ePermissionsWritable, 
                                                  allocate_error);
            
            if (mem == LLDB_INVALID_ADDRESS)
            {
                err.SetErrorStringWithFormat("Couldn't allocate a memory area to store %s: %s", var_sp->GetName().GetCString(), allocate_error.AsCString());
                return false;
            }
            
            if (log)
                log->Printf("Allocated %s (0x%llx) sucessfully", var_sp->GetName().GetCString(), mem);
            
            // Put the location of the spare memory into the live data of the ValueObject.
            
            var_sp->m_live_sp = ValueObjectConstResult::Create (exe_ctx.GetBestExecutionContextScope(),
                                                                var_sp->GetTypeFromUser().GetASTContext(),
                                                                var_sp->GetTypeFromUser().GetOpaqueQualType(),
                                                                var_sp->GetName(),
                                                                mem,
                                                                eAddressTypeLoad,
                                                                pvar_byte_size);
            
            // Clear the flag if the variable will never be deallocated.
            
            if (var_sp->m_flags & ClangExpressionVariable::EVKeepInTarget)
                var_sp->m_flags &= ~ClangExpressionVariable::EVNeedsAllocation;
            
            // Write the contents of the variable to the area.
            
            if (process->WriteMemory (mem, pvar_data, pvar_byte_size, error) != pvar_byte_size)
            {
                err.SetErrorStringWithFormat ("Couldn't write a composite type to the target: %s", error.AsCString());
                return false;
            }
        }
        
        if ((var_sp->m_flags & ClangExpressionVariable::EVIsProgramReference && var_sp->m_live_sp) ||
            var_sp->m_flags & ClangExpressionVariable::EVIsLLDBAllocated)
        {
            // Now write the location of the area into the struct.
            Error write_error;
            if (!process->WriteScalarToMemory (addr, 
                                                       var_sp->m_live_sp->GetValue().GetScalar(), 
                                                       process->GetAddressByteSize(), 
                                                       write_error))
            {
                err.SetErrorStringWithFormat ("Couldn't write %s to the target: %s", var_sp->GetName().GetCString(), write_error.AsCString());
                return false;
            }
            
            if (log)
                log->Printf("Materialized %s into 0x%llx", var_sp->GetName().GetCString(), var_sp->m_live_sp->GetValue().GetScalar().ULongLong());
        }
        else if (!(var_sp->m_flags & ClangExpressionVariable::EVIsProgramReference))
        {
            err.SetErrorStringWithFormat("Persistent variables without separate allocations are not currently supported.");
            return false;
        }
    }
    
    return true;
}

bool 
ClangExpressionDeclMap::DoMaterializeOneVariable
(
    bool dematerialize,
    ExecutionContext &exe_ctx,
    const SymbolContext &sym_ctx,
    ClangExpressionVariableSP &expr_var,
    lldb::addr_t addr, 
    Error &err
)
{
    lldb::LogSP log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_EXPRESSIONS));
    Target *target = exe_ctx.GetTargetPtr();
    Process *process = exe_ctx.GetProcessPtr();
    StackFrame *frame = exe_ctx.GetFramePtr();

    if (!frame || !process || !target || !m_parser_vars.get() || !expr_var->m_parser_vars.get())
        return false;
    
    // Vital information about the value
    
    const ConstString &name(expr_var->GetName());
    TypeFromUser type(expr_var->GetTypeFromUser());
    
    VariableSP &var(expr_var->m_parser_vars->m_lldb_var);
    lldb_private::Symbol *sym(expr_var->m_parser_vars->m_lldb_sym);
    
    std::auto_ptr<lldb_private::Value> location_value;

    if (var)
    {
        location_value.reset(GetVariableValue(exe_ctx,
                                              var,
                                              NULL));
    }
    else if (sym)
    {
        addr_t location_load_addr = GetSymbolAddress(*target, name);
        
        if (location_load_addr == LLDB_INVALID_ADDRESS)
        {
            if (log)
                err.SetErrorStringWithFormat ("Couldn't find value for global symbol %s", 
                                              name.GetCString());
        }
        
        location_value.reset(new Value);
        
        location_value->SetValueType(Value::eValueTypeLoadAddress);
        location_value->GetScalar() = location_load_addr;
    }
    else
    {
        err.SetErrorStringWithFormat ("Couldn't find %s with appropriate type", 
                                      name.GetCString());
        return false;
    }
    
    if (log)
    {
        StreamString my_stream_string;
        
        ClangASTType::DumpTypeDescription (type.GetASTContext(),
                                           type.GetOpaqueQualType(),
                                           &my_stream_string);
        
        log->Printf ("%s %s with type %s", 
                     dematerialize ? "Dematerializing" : "Materializing", 
                     name.GetCString(), 
                     my_stream_string.GetString().c_str());
    }
    
    if (!location_value.get())
    {
        err.SetErrorStringWithFormat("Couldn't get value for %s", name.GetCString());
        return false;
    }

    // The size of the type contained in addr
    
    size_t value_bit_size = ClangASTType::GetClangTypeBitWidth(type.GetASTContext(), type.GetOpaqueQualType());
    size_t value_byte_size = value_bit_size % 8 ? ((value_bit_size + 8) / 8) : (value_bit_size / 8);
    
    Value::ValueType value_type = location_value->GetValueType();
    
    switch (value_type)
    {
    default:
        {
            StreamString ss;
            
            location_value->Dump(&ss);
            
            err.SetErrorStringWithFormat ("%s has a value of unhandled type: %s", 
                                          name.GetCString(), 
                                          ss.GetString().c_str());
            return false;
        }
        break;
    case Value::eValueTypeLoadAddress:
        {
            if (!dematerialize)
            {
                Error write_error;

                if (!process->WriteScalarToMemory (addr, 
                                                           location_value->GetScalar(), 
                                                           process->GetAddressByteSize(), 
                                                           write_error))
                {
                    err.SetErrorStringWithFormat ("Couldn't write %s to the target: %s", 
                                                  name.GetCString(), 
                                                  write_error.AsCString());
                    return false;
                }
            }
        }
        break;
    case Value::eValueTypeScalar:
        {
            if (location_value->GetContextType() != Value::eContextTypeRegisterInfo)
            {
                StreamString ss;
                location_value->Dump(&ss);
                
                err.SetErrorStringWithFormat ("%s is a scalar of unhandled type: %s", 
                                              name.GetCString(), 
                                              ss.GetString().c_str());
                return false;
            }
            
            RegisterInfo *reg_info = location_value->GetRegisterInfo();
            
            if (!reg_info)
            {
                err.SetErrorStringWithFormat ("Couldn't get the register information for %s", 
                                              name.GetCString());
                return false;
            }
            
            RegisterValue reg_value;

            RegisterContext *reg_ctx = exe_ctx.GetRegisterContext();
            
            if (!reg_ctx)
            {
                err.SetErrorStringWithFormat ("Couldn't read register context to read %s from %s", 
                                              name.GetCString(), 
                                              reg_info->name);
                return false;
            }
            
            uint32_t register_byte_size = reg_info->byte_size;
            
            if (dematerialize)
            {
                // Get the location of the spare memory area out of the variable's live data.
                
                if (!expr_var->m_live_sp)
                {
                    err.SetErrorStringWithFormat("Couldn't find the memory area used to store %s", name.GetCString());
                    return false;
                }
                
                if (expr_var->m_live_sp->GetValue().GetValueAddressType() != eAddressTypeLoad)
                {
                    err.SetErrorStringWithFormat("The address of the memory area for %s is in an incorrect format", name.GetCString());
                    return false;
                }
                
                Scalar &reg_addr = expr_var->m_live_sp->GetValue().GetScalar();
                
                err = reg_ctx->ReadRegisterValueFromMemory (reg_info, 
                                                            reg_addr.ULongLong(), 
                                                            value_byte_size, 
                                                            reg_value);
                if (err.Fail())
                    return false;

                if (!reg_ctx->WriteRegister (reg_info, reg_value))
                {
                    err.SetErrorStringWithFormat ("Couldn't write %s to register %s", 
                                                  name.GetCString(), 
                                                  reg_info->name);
                    return false;
                }
                
                // Deallocate the spare area and clear the variable's live data.
                
                Error deallocate_error = process->DeallocateMemory(reg_addr.ULongLong());
                
                if (!deallocate_error.Success())
                {
                    err.SetErrorStringWithFormat ("Couldn't deallocate spare memory area for %s: %s", 
                                                  name.GetCString(), 
                                                  deallocate_error.AsCString());
                    return false;
                }
                
                expr_var->m_live_sp.reset();
            }
            else
            {
                // Allocate a spare memory area to place the register's contents into.  This memory area will be pointed to by the slot in the
                // struct.
                
                Error allocate_error;
                
                Scalar reg_addr (process->AllocateMemory (value_byte_size, 
                                                                  lldb::ePermissionsReadable | lldb::ePermissionsWritable, 
                                                                  allocate_error));
                
                if (reg_addr.ULongLong() == LLDB_INVALID_ADDRESS)
                {
                    err.SetErrorStringWithFormat ("Couldn't allocate a memory area to store %s: %s", 
                                                  name.GetCString(), 
                                                  allocate_error.AsCString());
                    return false;
                }
                
                // Put the location of the spare memory into the live data of the ValueObject.
                
                expr_var->m_live_sp = ValueObjectConstResult::Create (exe_ctx.GetBestExecutionContextScope(),
                                                                      type.GetASTContext(),
                                                                      type.GetOpaqueQualType(),
                                                                      name,
                                                                      reg_addr.ULongLong(),
                                                                      eAddressTypeLoad,
                                                                      value_byte_size);
                
                // Now write the location of the area into the struct.
                
                Error write_error;
                
                if (!process->WriteScalarToMemory (addr, 
                                                           reg_addr, 
                                                           process->GetAddressByteSize(), 
                                                           write_error))
                {
                    err.SetErrorStringWithFormat ("Couldn't write %s to the target: %s", 
                                                  name.GetCString(), 
                                                  write_error.AsCString());
                    return false;
                }
                
                if (value_byte_size > register_byte_size)
                {
                    err.SetErrorStringWithFormat ("%s is too big to store in %s", 
                                                  name.GetCString(), 
                                                  reg_info->name);
                    return false;
                }

                RegisterValue reg_value;

                if (!reg_ctx->ReadRegister (reg_info, reg_value))
                {
                    err.SetErrorStringWithFormat ("Couldn't read %s from %s", 
                                                  name.GetCString(), 
                                                  reg_info->name);
                    return false;
                }
                
                err = reg_ctx->WriteRegisterValueToMemory (reg_info, 
                                                           reg_addr.ULongLong(), 
                                                           value_byte_size, 
                                                           reg_value);
                if (err.Fail())
                    return false;
            }
        }
    }
    
    return true;
}

bool 
ClangExpressionDeclMap::DoMaterializeOneRegister
(
    bool dematerialize,
    ExecutionContext &exe_ctx,
    RegisterContext &reg_ctx,
    const RegisterInfo &reg_info,
    lldb::addr_t addr, 
    Error &err
)
{
    uint32_t register_byte_size = reg_info.byte_size;
    RegisterValue reg_value;
    if (dematerialize)
    {
        Error read_error (reg_ctx.ReadRegisterValueFromMemory(&reg_info, addr, register_byte_size, reg_value));
        if (read_error.Fail())
        {
            err.SetErrorStringWithFormat ("Couldn't read %s from the target: %s", reg_info.name, read_error.AsCString());
            return false;
        }
        
        if (!reg_ctx.WriteRegister (&reg_info, reg_value))
        {
            err.SetErrorStringWithFormat("Couldn't write register %s (dematerialize)", reg_info.name);
            return false;
        }
    }
    else
    {
        
        if (!reg_ctx.ReadRegister(&reg_info, reg_value))
        {
            err.SetErrorStringWithFormat("Couldn't read %s (materialize)", reg_info.name);
            return false;
        }
        
        Error write_error (reg_ctx.WriteRegisterValueToMemory(&reg_info, addr, register_byte_size, reg_value));
        if (write_error.Fail())
        {
            err.SetErrorStringWithFormat ("Couldn't write %s to the target: %s", reg_info.name, write_error.AsCString());
            return false;
        }
    }
    
    return true;
}

lldb::VariableSP
ClangExpressionDeclMap::FindVariableInScope
(
    StackFrame &frame,
    const ConstString &name,
    TypeFromUser *type
)
{    
    lldb::LogSP log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_EXPRESSIONS));
    
    ValueObjectSP valobj;
    VariableSP var_sp;
    Error err;
    
    valobj = frame.GetValueForVariableExpressionPath(name.GetCString(), 
                                                     eNoDynamicValues, 
                                                     StackFrame::eExpressionPathOptionCheckPtrVsMember,
                                                     var_sp,
                                                     err);
        
    if (!err.Success() ||
        !var_sp ||
        !var_sp->IsInScope(&frame) ||
        !var_sp->LocationIsValidForFrame (&frame))
        return lldb::VariableSP();

    if (var_sp && type)
    {
        if (type->GetASTContext() == var_sp->GetType()->GetClangAST())
        {
            if (!ClangASTContext::AreTypesSame(type->GetASTContext(), type->GetOpaqueQualType(), var_sp->GetType()->GetClangFullType()))
                return lldb::VariableSP();
        }
        else
        {
            if (log)
                log->PutCString("Skipping a candidate variable because of different AST contexts");
            return lldb::VariableSP();
        }
    }

    return var_sp;
}

Symbol *
ClangExpressionDeclMap::FindGlobalDataSymbol
(
    Target &target,
    const ConstString &name
)
{
    SymbolContextList sc_list;
    
    target.GetImages().FindSymbolsWithNameAndType(name, 
                                                  eSymbolTypeData, 
                                                  sc_list);
    
    if (sc_list.GetSize())
    {
        SymbolContext sym_ctx;
        sc_list.GetContextAtIndex(0, sym_ctx);
        
        return sym_ctx.symbol;
    }
    
    return NULL;
}

lldb::VariableSP
ClangExpressionDeclMap::FindGlobalVariable
(
    Target &target,
    ModuleSP &module,
    const ConstString &name,
    ClangNamespaceDecl *namespace_decl,
    TypeFromUser *type
)
{
    VariableList vars;
    
    if (module && namespace_decl)
        module->FindGlobalVariables (name, namespace_decl, true, -1, vars);
    else
        target.GetImages().FindGlobalVariables(name, true, -1, vars);
    
    if (vars.GetSize())
    {
        if (type)
        {
            for (size_t i = 0; i < vars.GetSize(); ++i)
            {
                VariableSP var_sp = vars.GetVariableAtIndex(i);
                
                if (type->GetASTContext() == var_sp->GetType()->GetClangAST())
                {
                    if (ClangASTContext::AreTypesSame(type->GetASTContext(), type->GetOpaqueQualType(), var_sp->GetType()->GetClangFullType()))
                        return var_sp;
                }
            }
        }
        else
        {
            return vars.GetVariableAtIndex(0);
        }
    }
    
    return VariableSP();
}

// Interface for ClangASTSource

void
ClangExpressionDeclMap::FindExternalVisibleDecls (NameSearchContext &context, const ConstString &name)
{
    lldb::LogSP log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_EXPRESSIONS));
    
    if (m_parser_vars->m_ignore_lookups)
    {
        if (log && log->GetVerbose())
            log->Printf("Ignoring a query during an import");
        return;
    }
    
    static unsigned int invocation_id = 0;
    unsigned int current_id = invocation_id++;
    
    if (log)
    {
        if (!context.m_decl_context)
            log->Printf("FindExternalVisibleDecls[%u] for '%s' in a NULL DeclContext", current_id, name.GetCString());
        else if (const NamedDecl *context_named_decl = dyn_cast<NamedDecl>(context.m_decl_context))
            log->Printf("FindExternalVisibleDecls[%u] for '%s' in '%s'", current_id, name.GetCString(), context_named_decl->getNameAsString().c_str());
        else
            log->Printf("FindExternalVisibleDecls[%u] for '%s' in a '%s'", current_id, name.GetCString(), context.m_decl_context->getDeclKindName());
    }
    
    context.m_namespace_map.reset(new ClangASTImporter::NamespaceMap);
        
    if (const NamespaceDecl *namespace_context = dyn_cast<NamespaceDecl>(context.m_decl_context))
    {
        ClangASTImporter::NamespaceMapSP namespace_map = m_parser_vars->m_ast_importer->GetNamespaceMap(namespace_context);
        
        if (log && log->GetVerbose())
            log->Printf("  FEVD[%u] Inspecting namespace map %p (%d entries)", 
                        current_id, 
                        namespace_map.get(), 
                        (int)namespace_map->size());
        
        for (ClangASTImporter::NamespaceMap::iterator i = namespace_map->begin(), e = namespace_map->end();
             i != e;
             ++i)
        {
            if (log)
                log->Printf("  FEVD[%u] Searching namespace %s in module %s",
                            current_id,
                            i->second.GetNamespaceDecl()->getNameAsString().c_str(),
                            i->first->GetFileSpec().GetFilename().GetCString());
                
            FindExternalVisibleDecls(context,
                                     i->first,
                                     i->second,
                                     name,
                                     current_id);
        }
    }
    else if (!isa<TranslationUnitDecl>(context.m_decl_context))
    {
        // we shouldn't be getting FindExternalVisibleDecls calls for these
        return;
    }
    else
    {
        ClangNamespaceDecl namespace_decl;
        
        if (log)
            log->Printf("  FEVD[%u] Searching the root namespace", current_id);
        
        FindExternalVisibleDecls(context,
                                 lldb::ModuleSP(),
                                 namespace_decl,
                                 name,
                                 current_id);
    }
    
    if (!context.m_namespace_map->empty())
    {
        if (log && log->GetVerbose())
            log->Printf("  FEVD[%u] Registering namespace map %p (%d entries)", 
                        current_id,
                        context.m_namespace_map.get(), 
                        (int)context.m_namespace_map->size());
        
        NamespaceDecl *clang_namespace_decl = AddNamespace(context, context.m_namespace_map);
        
        if (clang_namespace_decl)
            clang_namespace_decl->setHasExternalVisibleStorage();
    }
}

void 
ClangExpressionDeclMap::FindExternalVisibleDecls (NameSearchContext &context, 
                                                  lldb::ModuleSP module_sp,
                                                  ClangNamespaceDecl &namespace_decl,
                                                  const ConstString &name,
                                                  unsigned int current_id)
{
    assert (m_struct_vars.get());
    assert (m_parser_vars.get());
    
    lldb::LogSP log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_EXPRESSIONS));
            
    SymbolContextList sc_list;
    
    const char *name_unique_cstr = name.GetCString();
    
    if (name_unique_cstr == NULL)
        return;

    // Only look for functions by name out in our symbols if the function 
    // doesn't start with our phony prefix of '$'
    Target *target = m_parser_vars->m_exe_ctx->GetTargetPtr();
    StackFrame *frame = m_parser_vars->m_exe_ctx->GetFramePtr();
    if (name_unique_cstr[0] == '$' && !namespace_decl)
    {
        static ConstString g_lldb_class_name ("$__lldb_class");
        
        if (name == g_lldb_class_name)
        {
            // Clang is looking for the type of "this"
            
            if (!frame)
                return;
            
            VariableList *vars = frame->GetVariableList(false);
            
            if (!vars)
                return;
            
            lldb::VariableSP this_var = vars->FindVariable(ConstString("this"));
            
            if (!this_var ||
                !this_var->IsInScope(frame) || 
                !this_var->LocationIsValidForFrame (frame))
                return;
            
            Type *this_type = this_var->GetType();
            
            if (!this_type)
                return;
            
            if (log && log->GetVerbose())
            {
                log->Printf ("  FEVD[%u] Type for \"this\" is: ", current_id);
                StreamString strm;
                this_type->Dump(&strm, true);
                log->PutCString (strm.GetData());
            }
            
            TypeFromUser this_user_type(this_type->GetClangFullType(),
                                        this_type->GetClangAST());
            
            m_struct_vars->m_object_pointer_type = this_user_type;
            
            void *pointer_target_type = NULL;
            
            if (!ClangASTContext::IsPointerType(this_user_type.GetOpaqueQualType(),
                                                &pointer_target_type))
                return;
            
            clang::QualType pointer_target_qual_type = QualType::getFromOpaquePtr(pointer_target_type);
            
            if (pointer_target_qual_type.isConstQualified())
                pointer_target_qual_type.removeLocalConst();
            
            TypeFromUser class_user_type(pointer_target_qual_type.getAsOpaquePtr(),
                                         this_type->GetClangAST());
            
            if (log)
            {
                ASTDumper ast_dumper(pointer_target_qual_type);
                log->Printf("  FEVD[%u] Adding type for $__lldb_class: %s", current_id, ast_dumper.GetCString());
            }
            
            AddOneType(context, class_user_type, current_id, true);
            
            return;
        }
        
        static ConstString g_lldb_objc_class_name ("$__lldb_objc_class");
        if (name == g_lldb_objc_class_name)
        {
            // Clang is looking for the type of "*self"
            
            if (!frame)
                return;
            
            VariableList *vars = frame->GetVariableList(false);
            
            if (!vars)
                return;
            
            lldb::VariableSP self_var = vars->FindVariable(ConstString("self"));
            
            if (!self_var || 
                !self_var->IsInScope(frame) || 
                !self_var->LocationIsValidForFrame (frame))
                return;
            
            Type *self_type = self_var->GetType();
            
            if (!self_type)
                return;
            
            TypeFromUser self_user_type(self_type->GetClangFullType(),
                                        self_type->GetClangAST());
            
            m_struct_vars->m_object_pointer_type = self_user_type;
            
            void *pointer_target_type = NULL;
            
            if (!ClangASTContext::IsPointerType(self_user_type.GetOpaqueQualType(),
                                                &pointer_target_type)
                || pointer_target_type == NULL)
                return;
            
            TypeFromUser class_user_type(pointer_target_type,
                                         self_type->GetClangAST());
            
            if (log)
            {
                ASTDumper ast_dumper(pointer_target_type);
                log->Printf("  FEVD[%u] Adding type for $__lldb_objc_class: %s", current_id, ast_dumper.GetCString());
            }
            
            AddOneType(context, class_user_type, current_id, false);
            
            return;
        }
        
        // any other $__lldb names should be weeded out now
        if (!::strncmp(name_unique_cstr, "$__lldb", sizeof("$__lldb") - 1))
            return;
        
        do
        {
            if (!target)
                break;
            
            ClangASTContext *scratch_clang_ast_context = target->GetScratchClangASTContext();
            
            if (!scratch_clang_ast_context)
                break;
            
            ASTContext *scratch_ast_context = scratch_clang_ast_context->getASTContext();
            
            if (!scratch_ast_context)
                break;
            
            TypeDecl *ptype_type_decl = m_parser_vars->m_persistent_vars->GetPersistentType(name);
            
            if (!ptype_type_decl)
                break;
            
            Decl *parser_ptype_decl = ClangASTContext::CopyDecl(context.GetASTContext(), scratch_ast_context, ptype_type_decl);
            
            if (!parser_ptype_decl)
                break;
            
            TypeDecl *parser_ptype_type_decl = dyn_cast<TypeDecl>(parser_ptype_decl);
            
            if (!parser_ptype_type_decl)
                break;
            
            if (log)
                log->Printf("  FEVD[%u] Found persistent type %s", current_id, name.GetCString());
            
            context.AddNamedDecl(parser_ptype_type_decl);
        } while (0);
        
        ClangExpressionVariableSP pvar_sp(m_parser_vars->m_persistent_vars->GetVariable(name));
        
        if (pvar_sp)
        {
            AddOneVariable(context, pvar_sp, current_id);
            return;
        }
        
        const char *reg_name(&name.GetCString()[1]);
        
        if (m_parser_vars->m_exe_ctx->GetRegisterContext())
        {
            const RegisterInfo *reg_info(m_parser_vars->m_exe_ctx->GetRegisterContext()->GetRegisterInfoByName(reg_name));
                        
            if (reg_info)
            {
                if (log)
                    log->Printf("  FEVD[%u] Found register %s", current_id, reg_info->name);
                
                AddOneRegister(context, reg_info, current_id);
            }
        }
    }
    else
    {
        ValueObjectSP valobj;
        VariableSP var;
        Error err;
        
        if (frame && !namespace_decl)
        {
            valobj = frame->GetValueForVariableExpressionPath(name_unique_cstr, 
                                                              eNoDynamicValues, 
                                                              StackFrame::eExpressionPathOptionCheckPtrVsMember,
                                                              var,
                                                              err);
            
            // If we found a variable in scope, no need to pull up function names
            if (err.Success() && var != NULL)
            {
                AddOneVariable(context, var, current_id);
                context.m_found.variable = true;
            }
        }
        else if (target)
        {
            var = FindGlobalVariable (*target,
                                      module_sp,
                                      name,
                                      &namespace_decl,
                                      NULL);
            
            if (var)
            {
                AddOneVariable(context, var, current_id);
                context.m_found.variable = true;
            }
        }
        
        if (!context.m_found.variable)
        {
            const bool include_symbols = true;
            const bool append = false;
            
            if (namespace_decl && module_sp)
            {
                module_sp->FindFunctions(name,
                                         &namespace_decl,
                                         eFunctionNameTypeBase, 
                                         include_symbols,
                                         append,
                                         sc_list);
            }
            else
            {
                target->GetImages().FindFunctions(name,
                                                  eFunctionNameTypeBase,
                                                  include_symbols,
                                                  append, 
                                                  sc_list);
            }
            
            if (sc_list.GetSize())
            {
                Symbol *generic_symbol = NULL;
                Symbol *non_extern_symbol = NULL;
                
                for (uint32_t index = 0, num_indices = sc_list.GetSize();
                     index < num_indices;
                     ++index)
                {
                    SymbolContext sym_ctx;
                    sc_list.GetContextAtIndex(index, sym_ctx);
                    
                    if (sym_ctx.function)
                    {
                        // TODO only do this if it's a C function; C++ functions may be
                        // overloaded
                        if (!context.m_found.function_with_type_info)
                            AddOneFunction(context, sym_ctx.function, NULL, current_id);
                        context.m_found.function_with_type_info = true;
                        context.m_found.function = true;
                    }
                    else if (sym_ctx.symbol)
                    {
                        if (sym_ctx.symbol->IsExternal())
                            generic_symbol = sym_ctx.symbol;
                        else
                            non_extern_symbol = sym_ctx.symbol;
                    }
                }
                
                if (!context.m_found.function_with_type_info)
                {
                    if (generic_symbol)
                    {
                        AddOneFunction (context, NULL, generic_symbol, current_id);
                        context.m_found.function = true;
                    }
                    else if (non_extern_symbol)
                    {
                        AddOneFunction (context, NULL, non_extern_symbol, current_id);
                        context.m_found.function = true;
                    }
                }
            }
            
            if (!context.m_found.variable)
            {
                // We couldn't find a non-symbol variable for this.  Now we'll hunt for a generic 
                // data symbol, and -- if it is found -- treat it as a variable.
                
                Symbol *data_symbol = FindGlobalDataSymbol(*target, name);
                
                if (data_symbol)
                {
                    AddOneGenericVariable(context, *data_symbol, current_id);
                    context.m_found.variable = true;
                }
            }
        }
        
        if (module_sp && namespace_decl)
        {
            ClangNamespaceDecl found_namespace_decl;
            
            SymbolVendor *symbol_vendor = module_sp->GetSymbolVendor();
            
            if (symbol_vendor)
            {
                SymbolContext null_sc;
                
                found_namespace_decl = symbol_vendor->FindNamespace(null_sc, name, &namespace_decl);
                
                if (found_namespace_decl)
                {
                    context.m_namespace_map->push_back(std::pair<ModuleSP, ClangNamespaceDecl>(module_sp, found_namespace_decl));
                    
                    if (log)
                        log->Printf("  FEVD[%u] Found namespace %s in module %s",
                                    current_id,
                                    name.GetCString(), 
                                    module_sp->GetFileSpec().GetFilename().GetCString());
                }
            }
        }
        else 
        {
            ModuleList &images = m_parser_vars->m_sym_ctx.target_sp->GetImages();
                        
            for (uint32_t i = 0, e = images.GetSize();
                 i != e;
                 ++i)
            {
                ModuleSP image = images.GetModuleAtIndex(i);
                
                if (!image)
                    continue;
                
                ClangNamespaceDecl found_namespace_decl;
                
                SymbolVendor *symbol_vendor = image->GetSymbolVendor();
                
                if (!symbol_vendor)
                    continue;
                
                SymbolContext null_sc;
                
                found_namespace_decl = symbol_vendor->FindNamespace(null_sc, name, &namespace_decl);
                
                if (found_namespace_decl)
                {
                    context.m_namespace_map->push_back(std::pair<ModuleSP, ClangNamespaceDecl>(image, found_namespace_decl));
                    
                    if (log)
                        log->Printf("  FEVD[%u] Found namespace %s in module %s",
                                    current_id,
                                    name.GetCString(), 
                                    image->GetFileSpec().GetFilename().GetCString());
                }
            }
        }
    }    
    
    TypeList types;
    SymbolContext null_sc;
    
    if (module_sp && namespace_decl)
        module_sp->FindTypes(null_sc, name, &namespace_decl, true, 1, types);
    else
        target->GetImages().FindTypes (null_sc, name, true, 1, types);
    
    if (types.GetSize())
    {
        TypeSP type_sp = types.GetTypeAtIndex(0);
        
        if (log)
        {
            const char *name_string = type_sp->GetName().GetCString();
            
            log->Printf("  FEVD[%u] Matching type found for \"%s\": %s", 
                        current_id, 
                        name.GetCString(), 
                        (name_string ? name_string : "<anonymous>"));
        }

        TypeFromUser user_type(type_sp->GetClangFullType(),
                               type_sp->GetClangAST());
            
        AddOneType(context, user_type, current_id, false);
    }
}

clang::ExternalLoadResult
ClangExpressionDeclMap::FindExternalLexicalDecls (const DeclContext *decl_context, 
                                                  bool (*predicate)(Decl::Kind),
                                                  llvm::SmallVectorImpl<Decl*> &decls)
{
    assert (m_parser_vars.get());
    
    lldb::LogSP log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_EXPRESSIONS));
        
    const Decl *context_decl = dyn_cast<Decl>(decl_context);
    
    if (!context_decl)
        return ELR_Failure;
    
    ASTContext *ast_context = &context_decl->getASTContext();
    
    static unsigned int invocation_id = 0;
    unsigned int current_id = invocation_id++;
    
    if (log)
    {
        if (const NamedDecl *context_named_decl = dyn_cast<NamedDecl>(context_decl))
            log->Printf("FindExternalLexicalDecls[%u] in '%s' (a %s) with %s predicate",
                        current_id,
                        context_named_decl->getNameAsString().c_str(),
                        context_decl->getDeclKindName(), 
                        (predicate ? "non-null" : "null"));
        else if(context_decl)
            log->Printf("FindExternalLexicalDecls[%u] in a %s with %s predicate",
                        current_id,
                        context_decl->getDeclKindName(), 
                        (predicate ? "non-null" : "null"));
        else
            log->Printf("FindExternalLexicalDecls[%u] in a NULL context with %s predicate",
                        current_id,
                        (predicate ? "non-null" : "null"));
    }
    
    Decl *original_decl = NULL;
    ASTContext *original_ctx = NULL;
    
    ClangASTImporter *ast_importer = m_parser_vars->GetASTImporter(ast_context);
    
    if (!ast_importer)
        return ELR_Failure;
    
    if (!ast_importer->ResolveDeclOrigin(context_decl, &original_decl, &original_ctx))
        return ELR_Failure;
    
    if (log)
    {       
        log->Printf("  FELD[%u] Original decl:", current_id);
        ASTDumper(original_decl).ToLog(log, "    ");
    }
    
    if (TagDecl *original_tag_decl = dyn_cast<TagDecl>(original_decl))
    {
        ExternalASTSource *external_source = original_ctx->getExternalSource();
        
        if (external_source)
            external_source->CompleteType (original_tag_decl);
    }
    
    DeclContext *original_decl_context = dyn_cast<DeclContext>(original_decl);
    
    if (!original_decl_context)
        return ELR_Failure;
    
    for (TagDecl::decl_iterator iter = original_decl_context->decls_begin();
         iter != original_decl_context->decls_end();
         ++iter)
    {
        Decl *decl = *iter;
        
        if (!predicate || predicate(decl->getKind()))
        {
            if (log)
            {
                ASTDumper ast_dumper(decl);
                if (const NamedDecl *context_named_decl = dyn_cast<NamedDecl>(context_decl))
                    log->Printf("  FELD[%d] Adding [to %s] lexical decl %s", current_id, context_named_decl->getNameAsString().c_str(), ast_dumper.GetCString());
                else
                    log->Printf("  FELD[%d] Adding lexical decl %s", current_id, ast_dumper.GetCString());
            }
                        
            Decl *copied_decl = ast_importer->CopyDecl(original_ctx, decl);
            
            decls.push_back(copied_decl);
        }
    }
    
    return ELR_AlreadyLoaded;
}

void
ClangExpressionDeclMap::CompleteTagDecl (TagDecl *tag_decl)
{    
    assert (m_parser_vars.get());
    
    m_parser_vars->GetASTImporter(&tag_decl->getASTContext())->CompleteTagDecl (tag_decl);
}

void
ClangExpressionDeclMap::CompleteObjCInterfaceDecl (clang::ObjCInterfaceDecl *interface_decl)
{
    assert (m_parser_vars.get());
    
    m_parser_vars->GetASTImporter(&interface_decl->getASTContext())->CompleteObjCInterfaceDecl (interface_decl);
}

Value *
ClangExpressionDeclMap::GetVariableValue
(
    ExecutionContext &exe_ctx,
    VariableSP &var,
    ASTContext *parser_ast_context,
    TypeFromUser *user_type,
    TypeFromParser *parser_type
)
{
    lldb::LogSP log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_EXPRESSIONS));
    
    Type *var_type = var->GetType();
    
    if (!var_type) 
    {
        if (log)
            log->PutCString("Skipped a definition because it has no type");
        return NULL;
    }
    
    clang_type_t var_opaque_type = var_type->GetClangFullType();
    
    if (!var_opaque_type)
    {
        if (log)
            log->PutCString("Skipped a definition because it has no Clang type");
        return NULL;
    }
    
    ASTContext *ast = var_type->GetClangASTContext().getASTContext();
    
    if (!ast)
    {
        if (log)
            log->PutCString("There is no AST context for the current execution context");
        return NULL;
    }
    
    DWARFExpression &var_location_expr = var->LocationExpression();
    
    std::auto_ptr<Value> var_location(new Value);
    
    lldb::addr_t loclist_base_load_addr = LLDB_INVALID_ADDRESS;
    
    Target *target = exe_ctx.GetTargetPtr();

    if (var_location_expr.IsLocationList())
    {
        SymbolContext var_sc;
        var->CalculateSymbolContext (&var_sc);
        loclist_base_load_addr = var_sc.function->GetAddressRange().GetBaseAddress().GetLoadAddress (target);
    }
    Error err;
    
    if (!var_location_expr.Evaluate(&exe_ctx, ast, NULL, NULL, NULL, loclist_base_load_addr, NULL, *var_location.get(), &err))
    {
        if (log)
            log->Printf("Error evaluating location: %s", err.AsCString());
        return NULL;
    }
        
    void *type_to_use = NULL;
    
    if (parser_ast_context)
    {
        type_to_use = GuardedCopyType(parser_ast_context, ast, var_opaque_type);
        
        if (!type_to_use)
        {
            if (log)
                log->Printf("Couldn't copy a variable's type into the parser's AST context");
            
            return NULL;
        }
        
        if (parser_type)
            *parser_type = TypeFromParser(type_to_use, parser_ast_context);
    }
    else
        type_to_use = var_opaque_type;
    
    if (var_location.get()->GetContextType() == Value::eContextTypeInvalid)
        var_location.get()->SetContext(Value::eContextTypeClangType, type_to_use);
    
    if (var_location.get()->GetValueType() == Value::eValueTypeFileAddress)
    {
        SymbolContext var_sc;
        var->CalculateSymbolContext(&var_sc);
        
        if (!var_sc.module_sp)
            return NULL;
        
        ObjectFile *object_file = var_sc.module_sp->GetObjectFile();
        
        if (!object_file)
            return NULL;
        
        Address so_addr(var_location->GetScalar().ULongLong(), object_file->GetSectionList());
        
        lldb::addr_t load_addr = so_addr.GetLoadAddress(target);
        
        if (load_addr != LLDB_INVALID_ADDRESS)
        {
            var_location->GetScalar() = load_addr;
            var_location->SetValueType(Value::eValueTypeLoadAddress);
        }
    }
    
    if (user_type)
        *user_type = TypeFromUser(var_opaque_type, ast);
    
    return var_location.release();
}

void
ClangExpressionDeclMap::AddOneVariable (NameSearchContext &context, VariableSP var, unsigned int current_id)
{
    assert (m_parser_vars.get());
    
    lldb::LogSP log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_EXPRESSIONS));
        
    TypeFromUser ut;
    TypeFromParser pt;
    
    Value *var_location = GetVariableValue (*m_parser_vars->m_exe_ctx, 
                                            var, 
                                            context.GetASTContext(),
                                            &ut,
                                            &pt);
    
    if (!var_location)
        return;
    
    NamedDecl *var_decl = context.AddVarDecl(ClangASTContext::CreateLValueReferenceType(pt.GetASTContext(), pt.GetOpaqueQualType()));
    std::string decl_name(context.m_decl_name.getAsString());
    ConstString entity_name(decl_name.c_str());
    ClangExpressionVariableSP entity(m_found_entities.CreateVariable (m_parser_vars->m_exe_ctx->GetBestExecutionContextScope (),
                                                                      entity_name, 
                                                                      ut,
                                                                      m_parser_vars->m_target_info.byte_order,
                                                                      m_parser_vars->m_target_info.address_byte_size));
    assert (entity.get());
    entity->EnableParserVars();
    entity->m_parser_vars->m_parser_type = pt;
    entity->m_parser_vars->m_named_decl  = var_decl;
    entity->m_parser_vars->m_llvm_value  = NULL;
    entity->m_parser_vars->m_lldb_value  = var_location;
    entity->m_parser_vars->m_lldb_var    = var;
    
    if (log)
    {
        ASTDumper ast_dumper(var_decl);        
        log->Printf("  FEVD[%u] Found variable %s, returned %s", current_id, decl_name.c_str(), ast_dumper.GetCString());
    }
}

void
ClangExpressionDeclMap::AddOneVariable(NameSearchContext &context,
                                       ClangExpressionVariableSP &pvar_sp, 
                                       unsigned int current_id)
{
    lldb::LogSP log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_EXPRESSIONS));
    
    TypeFromUser user_type (pvar_sp->GetTypeFromUser());
    
    TypeFromParser parser_type (GuardedCopyType(context.GetASTContext(), 
                                                user_type.GetASTContext(), 
                                                user_type.GetOpaqueQualType()),
                                context.GetASTContext());
    
    NamedDecl *var_decl = context.AddVarDecl(ClangASTContext::CreateLValueReferenceType(parser_type.GetASTContext(), parser_type.GetOpaqueQualType()));
    
    pvar_sp->EnableParserVars();
    pvar_sp->m_parser_vars->m_parser_type = parser_type;
    pvar_sp->m_parser_vars->m_named_decl  = var_decl;
    pvar_sp->m_parser_vars->m_llvm_value  = NULL;
    pvar_sp->m_parser_vars->m_lldb_value  = NULL;
    
    if (log)
    {
        ASTDumper ast_dumper(var_decl);
        log->Printf("  FEVD[%u] Added pvar %s, returned %s", current_id, pvar_sp->GetName().GetCString(), ast_dumper.GetCString());
    }
}

void
ClangExpressionDeclMap::AddOneGenericVariable(NameSearchContext &context, 
                                              Symbol &symbol, 
                                              unsigned int current_id)
{
    assert(m_parser_vars.get());
    
    lldb::LogSP log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_EXPRESSIONS));
    
    Target *target = m_parser_vars->m_exe_ctx->GetTargetPtr();

    if (target == NULL)
        return;

    ASTContext *scratch_ast_context = target->GetScratchClangASTContext()->getASTContext();
    
    TypeFromUser user_type (ClangASTContext::CreateLValueReferenceType(scratch_ast_context, ClangASTContext::GetVoidPtrType(scratch_ast_context, true)),
                            scratch_ast_context);
    
    TypeFromParser parser_type (ClangASTContext::CreateLValueReferenceType(scratch_ast_context, ClangASTContext::GetVoidPtrType(context.GetASTContext(), true)),
                                context.GetASTContext());
    
    NamedDecl *var_decl = context.AddVarDecl(parser_type.GetOpaqueQualType());
    
    std::string decl_name(context.m_decl_name.getAsString());
    ConstString entity_name(decl_name.c_str());
    ClangExpressionVariableSP entity(m_found_entities.CreateVariable (m_parser_vars->m_exe_ctx->GetBestExecutionContextScope (),
                                                                      entity_name, 
                                                                      user_type,
                                                                      m_parser_vars->m_target_info.byte_order,
                                                                      m_parser_vars->m_target_info.address_byte_size));
    assert (entity.get());
    
    std::auto_ptr<Value> symbol_location(new Value);
    
    AddressRange &symbol_range = symbol.GetAddressRangeRef();
    Address &symbol_address = symbol_range.GetBaseAddress();
    lldb::addr_t symbol_load_addr = symbol_address.GetLoadAddress(target);
    
    symbol_location->SetContext(Value::eContextTypeClangType, user_type.GetOpaqueQualType());
    symbol_location->GetScalar() = symbol_load_addr;
    symbol_location->SetValueType(Value::eValueTypeLoadAddress);
    
    entity->EnableParserVars();
    entity->m_parser_vars->m_parser_type = parser_type;
    entity->m_parser_vars->m_named_decl  = var_decl;
    entity->m_parser_vars->m_llvm_value  = NULL;
    entity->m_parser_vars->m_lldb_value  = symbol_location.release();
    entity->m_parser_vars->m_lldb_sym    = &symbol;
    //entity->m_flags                      |= ClangExpressionVariable::EVUnknownType;
    
    if (log)
    {
        ASTDumper ast_dumper(var_decl);
        
        log->Printf("  FEVD[%u] Found variable %s, returned %s", current_id, decl_name.c_str(), ast_dumper.GetCString());
    }
}

bool 
ClangExpressionDeclMap::ResolveUnknownTypes()
{
    lldb::LogSP log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_EXPRESSIONS));
    Target *target = m_parser_vars->m_exe_ctx->GetTargetPtr();

    ASTContext *scratch_ast_context = target->GetScratchClangASTContext()->getASTContext();

    for (size_t index = 0, num_entities = m_found_entities.GetSize();
         index < num_entities;
         ++index)
    {
        ClangExpressionVariableSP entity = m_found_entities.GetVariableAtIndex(index);
        
        if (entity->m_flags & ClangExpressionVariable::EVUnknownType)
        {
            const NamedDecl *named_decl = entity->m_parser_vars->m_named_decl;
            const VarDecl *var_decl = dyn_cast<VarDecl>(named_decl);
            
            if (!var_decl)
            {
                if (log)
                    log->Printf("Entity of unknown type does not have a VarDecl");
                return false;
            }
            
            if (log)
            {
                ASTDumper ast_dumper(const_cast<VarDecl*>(var_decl));
                log->Printf("Variable of unknown type now has Decl %s", ast_dumper.GetCString());
            }
                
            QualType var_type = var_decl->getType();
            TypeFromParser parser_type(var_type.getAsOpaquePtr(), &var_decl->getASTContext());
            
            lldb::clang_type_t copied_type = ClangASTContext::CopyType(scratch_ast_context, &var_decl->getASTContext(), var_type.getAsOpaquePtr());
            
            TypeFromUser user_type(copied_type, scratch_ast_context);
                        
            entity->m_parser_vars->m_lldb_value->SetContext(Value::eContextTypeClangType, user_type.GetOpaqueQualType());
            entity->m_parser_vars->m_parser_type = parser_type;
            
            entity->SetClangAST(user_type.GetASTContext());
            entity->SetClangType(user_type.GetOpaqueQualType());
            
            entity->m_flags &= ~(ClangExpressionVariable::EVUnknownType);
        }
    }
            
    return true;
}

void
ClangExpressionDeclMap::AddOneRegister (NameSearchContext &context,
                                        const RegisterInfo *reg_info, 
                                        unsigned int current_id)
{
    lldb::LogSP log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_EXPRESSIONS));
    
    void *ast_type = ClangASTContext::GetBuiltinTypeForEncodingAndBitSize(context.GetASTContext(),
                                                                          reg_info->encoding,
                                                                          reg_info->byte_size * 8);
    
    if (!ast_type)
    {
        if (log)
            log->Printf("  Tried to add a type for %s, but couldn't get one", context.m_decl_name.getAsString().c_str());
        return;
    }
    
    TypeFromParser parser_type (ast_type,
                                context.GetASTContext());
    
    NamedDecl *var_decl = context.AddVarDecl(parser_type.GetOpaqueQualType());
    
    ClangExpressionVariableSP entity(m_found_entities.CreateVariable (m_parser_vars->m_exe_ctx->GetBestExecutionContextScope(),
                                                                      m_parser_vars->m_target_info.byte_order,
                                                                      m_parser_vars->m_target_info.address_byte_size));
    assert (entity.get());
    std::string decl_name(context.m_decl_name.getAsString());
    entity->SetName (ConstString (decl_name.c_str()));
    entity->SetRegisterInfo (reg_info);
    entity->EnableParserVars();
    entity->m_parser_vars->m_parser_type = parser_type;
    entity->m_parser_vars->m_named_decl  = var_decl;
    entity->m_parser_vars->m_llvm_value  = NULL;
    entity->m_parser_vars->m_lldb_value  = NULL;
    
    if (log && log->GetVerbose())
    {
        ASTDumper ast_dumper(var_decl);
        log->Printf("  FEVD[%d] Added register %s, returned %s", current_id, context.m_decl_name.getAsString().c_str(), ast_dumper.GetCString());
    }
}

NamespaceDecl *
ClangExpressionDeclMap::AddNamespace (NameSearchContext &context, ClangASTImporter::NamespaceMapSP &namespace_decls)
{
    if (namespace_decls.empty())
        return NULL;
    
    lldb::LogSP log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_EXPRESSIONS));
    
    assert (m_parser_vars.get());
    
    const ClangNamespaceDecl &namespace_decl = namespace_decls->begin()->second;
    
    Decl *copied_decl = m_parser_vars->GetASTImporter(context.GetASTContext())->CopyDecl(namespace_decl.GetASTContext(), 
                                                                                         namespace_decl.GetNamespaceDecl());
    
    NamespaceDecl *copied_namespace_decl = dyn_cast<NamespaceDecl>(copied_decl);
    
    m_parser_vars->GetASTImporter(context.GetASTContext())->RegisterNamespaceMap(copied_namespace_decl, namespace_decls);
    
    return dyn_cast<NamespaceDecl>(copied_decl);
}

void
ClangExpressionDeclMap::AddOneFunction (NameSearchContext &context,
                                        Function* fun,
                                        Symbol* symbol,
                                        unsigned int current_id)
{
    assert (m_parser_vars.get());
    
    lldb::LogSP log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_EXPRESSIONS));
    
    NamedDecl *fun_decl = NULL;
    std::auto_ptr<Value> fun_location(new Value);
    const Address *fun_address = NULL;
    
    // only valid for Functions, not for Symbols
    void *fun_opaque_type = NULL;
    ASTContext *fun_ast_context = NULL;
    
    if (fun)
    {
        Type *fun_type = fun->GetType();
        
        if (!fun_type) 
        {
            if (log)
                log->PutCString("  Skipped a function because it has no type");
            return;
        }
        
        fun_opaque_type = fun_type->GetClangFullType();
        
        if (!fun_opaque_type)
        {
            if (log)
                log->PutCString("  Skipped a function because it has no Clang type");
            return;
        }
        
        fun_address = &fun->GetAddressRange().GetBaseAddress();
        
        fun_ast_context = fun_type->GetClangASTContext().getASTContext();
        void *copied_type = GuardedCopyType(context.GetASTContext(), fun_ast_context, fun_opaque_type);
        if (copied_type)
        {
            fun_decl = context.AddFunDecl(copied_type);
        }
        else
        {
            // We failed to copy the type we found
            if (log)
            {
                log->Printf ("  Failed to import the function type '%s' {0x%8.8llx} into the expression parser AST contenxt",
                             fun_type->GetName().GetCString(), 
                             fun_type->GetID());
            }
        }
    }
    else if (symbol)
    {
        fun_address = &symbol->GetAddressRangeRef().GetBaseAddress();
        
        fun_decl = context.AddGenericFunDecl();
    }
    else
    {
        if (log)
            log->PutCString("  AddOneFunction called with no function and no symbol");
        return;
    }
    
    Target *target = m_parser_vars->m_exe_ctx->GetTargetPtr();

    lldb::addr_t load_addr = fun_address->GetCallableLoadAddress(target);
    fun_location->SetValueType(Value::eValueTypeLoadAddress);
    fun_location->GetScalar() = load_addr;
    
    ClangExpressionVariableSP entity(m_found_entities.CreateVariable (m_parser_vars->m_exe_ctx->GetBestExecutionContextScope (),
                                                                      m_parser_vars->m_target_info.byte_order,
                                                                      m_parser_vars->m_target_info.address_byte_size));
    assert (entity.get());
    std::string decl_name(context.m_decl_name.getAsString());
    entity->SetName(ConstString(decl_name.c_str()));
    entity->SetClangType (fun_opaque_type);
    entity->SetClangAST (fun_ast_context);
    
    entity->EnableParserVars();
    entity->m_parser_vars->m_named_decl  = fun_decl;
    entity->m_parser_vars->m_llvm_value  = NULL;
    entity->m_parser_vars->m_lldb_value  = fun_location.release();
        
    if (log)
    {
        ASTDumper ast_dumper(fun_decl);
        
        log->Printf("  FEVD[%u] Found %s function %s, returned %s", 
                    current_id,
                    (fun ? "specific" : "generic"), 
                    decl_name.c_str(), 
                    ast_dumper.GetCString());
    }
}

void 
ClangExpressionDeclMap::AddOneType(NameSearchContext &context, 
                                   TypeFromUser &ut,
                                   unsigned int current_id,
                                   bool add_method)
{
    ASTContext *parser_ast_context = context.GetASTContext();
    ASTContext *user_ast_context = ut.GetASTContext();
    
    void *copied_type = GuardedCopyType(parser_ast_context, user_ast_context, ut.GetOpaqueQualType());
 
    TypeFromParser parser_type(copied_type, parser_ast_context);
    
    if (add_method && ClangASTContext::IsAggregateType(copied_type))
    {
        void *args[1];
        
        args[0] = ClangASTContext::GetVoidPtrType(parser_ast_context, false);
        
        void *method_type = ClangASTContext::CreateFunctionType (parser_ast_context,
                                                                 ClangASTContext::GetBuiltInType_void(parser_ast_context),
                                                                 args,
                                                                 1,
                                                                 false,
                                                                 ClangASTContext::GetTypeQualifiers(copied_type));

        const bool is_virtual = false;
        const bool is_static = false;
        const bool is_inline = false;
        const bool is_explicit = false;
        
        ClangASTContext::AddMethodToCXXRecordType (parser_ast_context,
                                                   copied_type,
                                                   "$__lldb_expr",
                                                   method_type,
                                                   lldb::eAccessPublic,
                                                   is_virtual,
                                                   is_static,
                                                   is_inline,
                                                   is_explicit);
    }
    
    context.AddTypeDecl(copied_type);
}

void * 
ClangExpressionDeclMap::GuardedCopyType (ASTContext *dest_context, 
                                         ASTContext *source_context,
                                         void *clang_type)
{
    assert (m_parser_vars.get());
    
    m_parser_vars->m_ignore_lookups = true;
    
    lldb_private::ClangASTImporter *importer = m_parser_vars->GetASTImporter(dest_context);
    
    QualType ret_qual_type = importer->CopyType (source_context,
                                                 QualType::getFromOpaquePtr(clang_type));
    
    void *ret = ret_qual_type.getAsOpaquePtr();
    
    m_parser_vars->m_ignore_lookups = false;
    
    return ret;
}
