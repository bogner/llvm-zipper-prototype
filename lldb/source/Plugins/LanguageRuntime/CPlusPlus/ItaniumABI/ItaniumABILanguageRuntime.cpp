//===-- ItaniumABILanguageRuntime.cpp --------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "ItaniumABILanguageRuntime.h"

#include "lldb/Breakpoint/BreakpointLocation.h"
#include "lldb/Core/ConstString.h"
#include "lldb/Core/Error.h"
#include "lldb/Core/Module.h"
#include "lldb/Core/PluginManager.h"
#include "lldb/Core/Scalar.h"
#include "lldb/Core/ValueObject.h"
#include "lldb/Core/ValueObjectMemory.h"
#include "lldb/Symbol/ClangASTContext.h"
#include "lldb/Target/Process.h"
#include "lldb/Target/RegisterContext.h"
#include "lldb/Target/StopInfo.h"
#include "lldb/Target/Target.h"
#include "lldb/Target/Thread.h"

#include <vector>

using namespace lldb;
using namespace lldb_private;

static const char *pluginName = "ItaniumABILanguageRuntime";
static const char *pluginDesc = "Itanium ABI for the C++ language";
static const char *pluginShort = "language.itanium";
static const char *vtable_demangled_prefix = "vtable for ";

bool
ItaniumABILanguageRuntime::CouldHaveDynamicValue (ValueObject &in_value)
{
    return in_value.IsPossibleCPlusPlusDynamicType();
}

bool
ItaniumABILanguageRuntime::GetDynamicTypeAndAddress (ValueObject &in_value, 
                                                     lldb::DynamicValueType use_dynamic, 
                                                     TypeAndOrName &class_type_or_name, 
                                                     Address &dynamic_address)
{
    // For Itanium, if the type has a vtable pointer in the object, it will be at offset 0
    // in the object.  That will point to the "address point" within the vtable (not the beginning of the
    // vtable.)  We can then look up the symbol containing this "address point" and that symbol's name 
    // demangled will contain the full class name.
    // The second pointer above the "address point" is the "offset_to_top".  We'll use that to get the
    // start of the value object which holds the dynamic type.
    //
    
    // Only a pointer or reference type can have a different dynamic and static type:
    if (CouldHaveDynamicValue (in_value))
    {
        // FIXME: Can we get the Clang Type and ask it if the thing is really virtual?  That would avoid false positives,
        // at the cost of not looking for the dynamic type of objects if DWARF->Clang gets it wrong.
        
        // First job, pull out the address at 0 offset from the object.
        AddressType address_type;
        lldb::addr_t original_ptr = in_value.GetPointerValue(&address_type);
        if (original_ptr == LLDB_INVALID_ADDRESS)
            return false;
        
        ExecutionContext exe_ctx (in_value.GetExecutionContextRef());

        Target *target = exe_ctx.GetTargetPtr();
        Process *process = exe_ctx.GetProcessPtr();

        char memory_buffer[16];
        DataExtractor data(memory_buffer, sizeof(memory_buffer), 
                           process->GetByteOrder(), 
                           process->GetAddressByteSize());
        size_t address_byte_size = process->GetAddressByteSize();
        Error error;
        size_t bytes_read = process->ReadMemory (original_ptr, 
                                                 memory_buffer, 
                                                 address_byte_size, 
                                                 error);
        if (!error.Success() || (bytes_read != address_byte_size))
        {
            return false;
        }
        
        uint32_t offset_ptr = 0;
        lldb::addr_t vtable_address_point = data.GetAddress (&offset_ptr);
            
        if (offset_ptr == 0)
            return false;
        
        // Now find the symbol that contains this address:
        
        SymbolContext sc;
        Address address_point_address;
        if (target && !target->GetSectionLoadList().IsEmpty())
        {
            if (target->GetSectionLoadList().ResolveLoadAddress (vtable_address_point, address_point_address))
            {
                target->GetImages().ResolveSymbolContextForAddress (address_point_address, eSymbolContextSymbol, sc);
                Symbol *symbol = sc.symbol;
                if (symbol != NULL)
                {
                    const char *name = symbol->GetMangled().GetDemangledName().AsCString();
                    if (strstr(name, vtable_demangled_prefix) == name)
                    {
                         // We are a C++ class, that's good.  Get the class name and look it up:
                        const char *class_name = name + strlen(vtable_demangled_prefix);
                        class_type_or_name.SetName (class_name);
                        TypeList class_types;
                        uint32_t num_matches = target->GetImages().FindTypes (sc, 
                                                                              ConstString(class_name),
                                                                              true,
                                                                              UINT32_MAX,
                                                                              class_types);
                        if (num_matches == 1)
                        {
                            class_type_or_name.SetTypeSP(class_types.GetTypeAtIndex(0));
                        }
                        else if (num_matches > 1)
                        {
                            for (size_t i = 0; i < num_matches; i++)
                            {
                                lldb::TypeSP this_type(class_types.GetTypeAtIndex(i));
                                if (this_type)
                                {
                                    if (ClangASTContext::IsCXXClassType(this_type->GetClangFullType()))
                                    {
                                        // There can only be one type with a given name,
                                        // so we've just found duplicate definitions, and this
                                        // one will do as well as any other.
                                        // We don't consider something to have a dynamic type if
                                        // it is the same as the static type.  So compare against
                                        // the value we were handed:
                                        
                                        clang::ASTContext *in_ast_ctx = in_value.GetClangAST ();
                                        clang::ASTContext *this_ast_ctx = this_type->GetClangAST ();
                                        if (in_ast_ctx != this_ast_ctx
                                            || !ClangASTContext::AreTypesSame (in_ast_ctx, 
                                                                               in_value.GetClangType(),
                                                                               this_type->GetClangFullType()))
                                        {
                                            class_type_or_name.SetTypeSP (this_type);
                                            return true;
                                        }
                                        return false;
                                    }
                                }
                            }
                        }
                        else
                            return false;
                            
                        // The offset_to_top is two pointers above the address.
                        Address offset_to_top_address = address_point_address;
                        int64_t slide = -2 * ((int64_t) target->GetArchitecture().GetAddressByteSize());
                        offset_to_top_address.Slide (slide);
                        
                        Error error;
                        lldb::addr_t offset_to_top_location = offset_to_top_address.GetLoadAddress(target);
                        
                        size_t bytes_read = process->ReadMemory (offset_to_top_location, 
                                                                 memory_buffer, 
                                                                 address_byte_size, 
                                                                 error);
                                                                 
                        if (!error.Success() || (bytes_read != address_byte_size))
                        {
                            return false;
                        }
                        
                        offset_ptr = 0;
                        int64_t offset_to_top = data.GetMaxS64(&offset_ptr, process->GetAddressByteSize());
                        
                        // So the dynamic type is a value that starts at offset_to_top
                        // above the original address.
                        lldb::addr_t dynamic_addr = original_ptr + offset_to_top;
                        if (!target->GetSectionLoadList().ResolveLoadAddress (dynamic_addr, dynamic_address))
                        {
                            dynamic_address.SetRawAddress(dynamic_addr);
                        }
                        return true;
                    }
                }
            }
        }
        
    }
    
    return false;
}

bool
ItaniumABILanguageRuntime::IsVTableName (const char *name)
{
    if (name == NULL)
        return false;
        
    // Can we maybe ask Clang about this?
    if (strstr (name, "_vptr$") == name)
        return true;
    else
        return false;
}

//------------------------------------------------------------------
// Static Functions
//------------------------------------------------------------------
LanguageRuntime *
ItaniumABILanguageRuntime::CreateInstance (Process *process, lldb::LanguageType language)
{
    // FIXME: We have to check the process and make sure we actually know that this process supports
    // the Itanium ABI.
    if (language == eLanguageTypeC_plus_plus)
        return new ItaniumABILanguageRuntime (process);
    else
        return NULL;
}

void
ItaniumABILanguageRuntime::Initialize()
{
    PluginManager::RegisterPlugin (pluginName,
                                   pluginDesc,
                                   CreateInstance);    
}

void
ItaniumABILanguageRuntime::Terminate()
{
    PluginManager::UnregisterPlugin (CreateInstance);
}

//------------------------------------------------------------------
// PluginInterface protocol
//------------------------------------------------------------------
const char *
ItaniumABILanguageRuntime::GetPluginName()
{
    return pluginName;
}

const char *
ItaniumABILanguageRuntime::GetShortPluginName()
{
    return pluginShort;
}

uint32_t
ItaniumABILanguageRuntime::GetPluginVersion()
{
    return 1;
}

static const char *exception_names[] = { "__cxa_begin_catch", "__cxa_throw", "__cxa_rethrow", "__cxa_allocate_exception"};
static const int num_throw_names = 3;
static const int num_expression_throw_names = 1;

BreakpointResolverSP
ItaniumABILanguageRuntime::CreateExceptionResolver (Breakpoint *bkpt, bool catch_bp, bool throw_bp)
{
    return CreateExceptionResolver (bkpt, catch_bp, throw_bp, false);
}

BreakpointResolverSP
ItaniumABILanguageRuntime::CreateExceptionResolver (Breakpoint *bkpt, bool catch_bp, bool throw_bp, bool for_expressions)
{
    BreakpointResolverSP resolver_sp;
    static const int total_expressions = sizeof (exception_names)/sizeof (char *);
    
    // One complication here is that most users DON'T want to stop at __cxa_allocate_expression, but until we can do
    // anything better with predicting unwinding the expression parser does.  So we have two forms of the exception
    // breakpoints, one for expressions that leaves out __cxa_allocate_exception, and one that includes it.
    // The SetExceptionBreakpoints does the latter, the CreateExceptionBreakpoint in the runtime the former.
    
    uint32_t num_expressions;
    if (catch_bp && throw_bp)
    {
        if (for_expressions)
            num_expressions = total_expressions;
        else
            num_expressions = total_expressions - num_expression_throw_names;
            
        resolver_sp.reset (new BreakpointResolverName (bkpt,
                                                       exception_names,
                                                       num_expressions,
                                                       eFunctionNameTypeBase,
                                                       eLazyBoolNo));
    }
    else if (throw_bp)
    {
        if (for_expressions)
            num_expressions = num_throw_names - num_expression_throw_names;
        else
            num_expressions = num_throw_names;
            
        resolver_sp.reset (new BreakpointResolverName (bkpt,
                                                       exception_names + 1,
                                                       num_expressions,
                                                       eFunctionNameTypeBase,
                                                       eLazyBoolNo));
    }
    else if (catch_bp)
        resolver_sp.reset (new BreakpointResolverName (bkpt,
                                                       exception_names,
                                                       total_expressions - num_throw_names,
                                                       eFunctionNameTypeBase,
                                                       eLazyBoolNo));

    return resolver_sp;
}

void
ItaniumABILanguageRuntime::SetExceptionBreakpoints ()
{
    if (!m_process)
        return;
    
    const bool catch_bp = false;
    const bool throw_bp = true;
    const bool is_internal = true;
    const bool for_expressions = true;
    
    // For the exception breakpoints set by the Expression parser, we'll be a little more aggressive and
    // stop at exception allocation as well.
    
    if (!m_cxx_exception_bp_sp)
    {
        Target &target = m_process->GetTarget();
        
        BreakpointResolverSP exception_resolver_sp = CreateExceptionResolver (NULL, catch_bp, throw_bp, for_expressions);
        SearchFilterSP filter_sp = target.GetSearchFilterForModule(NULL);
        
        m_cxx_exception_bp_sp = target.CreateBreakpoint (filter_sp, exception_resolver_sp, is_internal);
    }
    else
        m_cxx_exception_bp_sp->SetEnabled (true);
    
}

void
ItaniumABILanguageRuntime::ClearExceptionBreakpoints ()
{
    if (!m_process)
        return;
    
    if (m_cxx_exception_bp_sp.get())
    {
        m_cxx_exception_bp_sp->SetEnabled (false);
    }    
}

bool
ItaniumABILanguageRuntime::ExceptionBreakpointsExplainStop (lldb::StopInfoSP stop_reason)
{
    if (!m_process)
        return false;
    
    if (!stop_reason || 
        stop_reason->GetStopReason() != eStopReasonBreakpoint)
        return false;
    
    uint64_t break_site_id = stop_reason->GetValue();
    lldb::BreakpointSiteSP bp_site_sp = m_process->GetBreakpointSiteList().FindByID(break_site_id);
    
    if (!bp_site_sp)
        return false;
    
    uint32_t num_owners = bp_site_sp->GetNumberOfOwners();
    
    break_id_t  cxx_exception_bid;
    
    if (!m_cxx_exception_bp_sp)
        return false;
        
    cxx_exception_bid = m_cxx_exception_bp_sp->GetID();
        
    for (uint32_t i = 0; i < num_owners; i++)
    {
        break_id_t bid = bp_site_sp->GetOwnerAtIndex(i)->GetBreakpoint().GetID();
        
        if (bid == cxx_exception_bid)
            return true;
    }
    
    return false;
}
