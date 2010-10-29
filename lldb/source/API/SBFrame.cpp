//===-- SBFrame.cpp ---------------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "lldb/API/SBFrame.h"

#include <string>
#include <algorithm>

#include "lldb/lldb-types.h"

#include "lldb/Core/Address.h"
#include "lldb/Core/ConstString.h"
#include "lldb/Core/Log.h"
#include "lldb/Core/Stream.h"
#include "lldb/Core/StreamFile.h"
#include "lldb/Core/ValueObjectRegister.h"
#include "lldb/Core/ValueObjectVariable.h"
#include "lldb/Expression/ClangUserExpression.h"
#include "lldb/Symbol/Block.h"
#include "lldb/Symbol/SymbolContext.h"
#include "lldb/Symbol/VariableList.h"
#include "lldb/Symbol/Variable.h"
#include "lldb/Target/ExecutionContext.h"
#include "lldb/Target/Target.h"
#include "lldb/Target/Process.h"
#include "lldb/Target/RegisterContext.h"
#include "lldb/Target/StackFrame.h"
#include "lldb/Target/Thread.h"

#include "lldb/API/SBDebugger.h"
#include "lldb/API/SBValue.h"
#include "lldb/API/SBAddress.h"
#include "lldb/API/SBStream.h"
#include "lldb/API/SBSymbolContext.h"
#include "lldb/API/SBThread.h"

using namespace lldb;
using namespace lldb_private;

SBFrame::SBFrame () :
    m_opaque_sp ()
{
}

SBFrame::SBFrame (const lldb::StackFrameSP &lldb_object_sp) :
    m_opaque_sp (lldb_object_sp)
{
    Log *log = lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_API);

    if (log)
    {
        SBStream sstr;
        GetDescription (sstr);
        log->Printf ("SBFrame::SBFrame (lldb_object_sp=%p) => this.sp = %p (%s)", lldb_object_sp.get(), 
                     m_opaque_sp.get(), sstr.GetData());
                     
    }
}

SBFrame::~SBFrame()
{
}


void
SBFrame::SetFrame (const lldb::StackFrameSP &lldb_object_sp)
{
    m_opaque_sp = lldb_object_sp;
}


bool
SBFrame::IsValid() const
{
    return (m_opaque_sp.get() != NULL);
}

SBSymbolContext
SBFrame::GetSymbolContext (uint32_t resolve_scope) const
{
    Log *log = lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_API);

    //if (log)
    //    log->Printf ("SBFrame::GetSymbolContext (this.sp=%p, resolve_scope=%d)", m_opaque_sp.get(), resolve_scope);

    SBSymbolContext sb_sym_ctx;
    if (m_opaque_sp)
        sb_sym_ctx.SetSymbolContext(&m_opaque_sp->GetSymbolContext (resolve_scope));

    if (log)
        log->Printf ("SBFrame::GetSymbolContext (this.sp=%p, resolve_scope=%d) => SBSymbolContext (this.ap = %p)", 
                     m_opaque_sp.get(), resolve_scope, sb_sym_ctx.get());

    return sb_sym_ctx;
}

SBModule
SBFrame::GetModule () const
{
    SBModule sb_module (m_opaque_sp->GetSymbolContext (eSymbolContextModule).module_sp);
    return sb_module;
}

SBCompileUnit
SBFrame::GetCompileUnit () const
{
    Log *log = lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_API);

    //if (log)
    //    log->Printf ("SBFrame::GetCompileUnit()");

    SBCompileUnit sb_comp_unit(m_opaque_sp->GetSymbolContext (eSymbolContextCompUnit).comp_unit);

    if (log)
        log->Printf ("SBFrame::GetCompileUnit (this.sp=%p) => SBCompileUnit (this=%p)", m_opaque_sp.get(), 
                     sb_comp_unit.get());

    return sb_comp_unit;
}

SBFunction
SBFrame::GetFunction () const
{
    SBFunction sb_function(m_opaque_sp->GetSymbolContext (eSymbolContextFunction).function);
    return sb_function;
}

SBSymbol
SBFrame::GetSymbol () const
{
    SBSymbol sb_symbol(m_opaque_sp->GetSymbolContext (eSymbolContextSymbol).symbol);
    return sb_symbol;
}

SBBlock
SBFrame::GetBlock () const
{
    SBBlock sb_block(m_opaque_sp->GetSymbolContext (eSymbolContextBlock).block);
    return sb_block;
}

SBBlock
SBFrame::GetFrameBlock () const
{
    SBBlock sb_block(m_opaque_sp->GetFrameBlock ());
    return sb_block;    
}

SBLineEntry
SBFrame::GetLineEntry () const
{
    SBLineEntry sb_line_entry(&m_opaque_sp->GetSymbolContext (eSymbolContextLineEntry).line_entry);
    return sb_line_entry;
}

uint32_t
SBFrame::GetFrameID () const
{
    if (m_opaque_sp)
        return m_opaque_sp->GetFrameIndex ();
    else
        return UINT32_MAX;
}

lldb::addr_t
SBFrame::GetPC () const
{
    Log *log = lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_API);

    //if (log)
    //    log->Printf ("SBFrame::GetPC (this.sp=%p)", m_opaque_sp.get());

    lldb::addr_t addr = LLDB_INVALID_ADDRESS;
    if (m_opaque_sp)
        addr = m_opaque_sp->GetFrameCodeAddress().GetLoadAddress (&m_opaque_sp->GetThread().GetProcess().GetTarget());

    if (log)
        log->Printf ("SBFrame::GetPC (this.sp=%p) => %p", m_opaque_sp.get(), addr);

    return addr;
}

bool
SBFrame::SetPC (lldb::addr_t new_pc)
{
    Log *log = lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_API);

    //if (log)
    //    log->Printf ("SBFrame::SetPC (this.sp=%p, new_pc=%p)", m_opaque_sp.get(), new_pc);

    bool ret_val = false;
    if (m_opaque_sp)
        ret_val = m_opaque_sp->GetRegisterContext()->SetPC (new_pc);

    if (log)
        log->Printf ("SBFrame::SetPC (this.sp=%p, new_pc=%p) => '%s'", m_opaque_sp.get(), new_pc, 
                     (ret_val ? "true" : "false"));

    return ret_val;
}

lldb::addr_t
SBFrame::GetSP () const
{
    if (m_opaque_sp)
        return m_opaque_sp->GetRegisterContext()->GetSP();
    return LLDB_INVALID_ADDRESS;
}


lldb::addr_t
SBFrame::GetFP () const
{
    Log *log = lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_API);

    //if (log)
    //    log->Printf ("SBFrame::GetFP ()");

    lldb::addr_t addr = LLDB_INVALID_ADDRESS;
    if (m_opaque_sp)
        addr = m_opaque_sp->GetRegisterContext()->GetFP();

    if (log)
        log->Printf ("SBFrame::GetFP (this.sp=%p) => %p", m_opaque_sp.get(), addr);

    return addr;
}


SBAddress
SBFrame::GetPCAddress () const
{
    SBAddress sb_addr;
    if (m_opaque_sp)
        sb_addr.SetAddress (&m_opaque_sp->GetFrameCodeAddress());
    return sb_addr;
}

void
SBFrame::Clear()
{
    m_opaque_sp.reset();
}

SBValue
SBFrame::LookupVar (const char *var_name)
{
    lldb::VariableSP var_sp;
    if (IsValid ())
    {
        lldb_private::VariableList variable_list;
        SBSymbolContext sc = GetSymbolContext (eSymbolContextEverything);

        SBBlock block = sc.GetBlock();
        if (block.IsValid())
            block.AppendVariables (true, true, &variable_list);

        const uint32_t num_variables = variable_list.GetSize();

        bool found = false;
        for (uint32_t i = 0; i < num_variables && !found; ++i)
        {
            var_sp = variable_list.GetVariableAtIndex(i);
            if (var_sp
                && (var_sp.get()->GetName() == lldb_private::ConstString(var_name)))
                found = true;
        }
        if (!found)
            var_sp.reset();
    }
    if (var_sp)
    {
        SBValue sb_value (ValueObjectSP (new ValueObjectVariable (var_sp)));
        return sb_value;
    }
    
    SBValue sb_value;
    return sb_value;
}

SBValue
SBFrame::LookupVarInScope (const char *var_name, const char *scope)
{
    lldb::VariableSP var_sp;
    if (IsValid())
    {
        std::string scope_str = scope;
        lldb::ValueType var_scope = eValueTypeInvalid;
        // Convert scope_str to be all lowercase;
        std::transform (scope_str.begin(), scope_str.end(), scope_str.begin(), ::tolower);

        if (scope_str.compare ("global") == 0)
            var_scope = eValueTypeVariableGlobal;
        else if (scope_str.compare ("local") == 0)
            var_scope = eValueTypeVariableLocal;
        else if (scope_str.compare ("parameter") == 0)
           var_scope = eValueTypeVariableArgument;

        if (var_scope != eValueTypeInvalid)
        {
            lldb_private::VariableList variable_list;
            SBSymbolContext sc = GetSymbolContext (eSymbolContextEverything);

            SBBlock block = sc.GetBlock();
            if (block.IsValid())
                block.AppendVariables (true, true, &variable_list);

            const uint32_t num_variables = variable_list.GetSize();

            bool found = false;
            for (uint32_t i = 0; i < num_variables && !found; ++i)
            {
                var_sp = variable_list.GetVariableAtIndex(i);
                if (var_sp
                    && (var_sp.get()->GetName() == lldb_private::ConstString(var_name))
                    && var_sp.get()->GetScope() == var_scope)
                    found = true;
            }
            if (!found)
                var_sp.reset();
        }
    }
    
    if (var_sp)
    {
        SBValue sb_value (ValueObjectSP (new ValueObjectVariable (var_sp)));
        return sb_value;
    }
    
    SBValue sb_value;
    return sb_value;
}

bool
SBFrame::operator == (const SBFrame &rhs) const
{
    return m_opaque_sp.get() == rhs.m_opaque_sp.get();
}

bool
SBFrame::operator != (const SBFrame &rhs) const
{
    return m_opaque_sp.get() != rhs.m_opaque_sp.get();
}

lldb_private::StackFrame *
SBFrame::operator->() const
{
    return m_opaque_sp.get();
}

lldb_private::StackFrame *
SBFrame::get() const
{
    return m_opaque_sp.get();
}


SBThread
SBFrame::GetThread () const
{
    Log *log = lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_API);

    //if (log)
    //    log->Printf ("SBFrame::GetThread ()");

    SBThread sb_thread (m_opaque_sp->GetThread().GetSP());

    if (log)
    {
        SBStream sstr;
        sb_thread.GetDescription (sstr);
        log->Printf ("SBFrame::GetThread (this.sp=%p) => SBThread : this.sp= %p, '%s'", m_opaque_sp.get(), 
                     sb_thread.GetLLDBObjectPtr(), sstr.GetData());
    }

    return sb_thread;
}

const char *
SBFrame::Disassemble () const
{
    Log *log = lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_API);
    Log *verbose_log = lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_API | LIBLLDB_LOG_VERBOSE);

    if (verbose_log)
        verbose_log->Printf ("SBFrame::Disassemble (this.sp=%p) => %s", m_opaque_sp.get(), m_opaque_sp->Disassemble());
    else if (log)
        log->Printf ("SBFrame::Disassemble (this.sp=%p)", m_opaque_sp.get());

    if (m_opaque_sp)
        return m_opaque_sp->Disassemble();
    return NULL;
}



lldb_private::StackFrame *
SBFrame::GetLLDBObjectPtr ()
{
    return m_opaque_sp.get();
}

SBValueList
SBFrame::GetVariables (bool arguments,
                       bool locals,
                       bool statics,
                       bool in_scope_only)
{
    Log *log = lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_API);

    if (log)
        log->Printf ("SBFrame::GetVariables (this_sp.get=%p, arguments=%s, locals=%s, statics=%s, in_scope_only=%s)", 
                     m_opaque_sp.get(), 
                     (arguments ? "true" : "false"),
                     (locals    ? "true" : "false"),
                     (statics   ? "true" : "false"),
                     (in_scope_only ? "true" : "false"));

    SBValueList value_list;
    if (m_opaque_sp)
    {
        size_t i;
        VariableList *variable_list = m_opaque_sp->GetVariableList(true);
        if (variable_list)
        {
            const size_t num_variables = variable_list->GetSize();
            if (num_variables)
            {
                for (i = 0; i < num_variables; ++i)
                {
                    VariableSP variable_sp (variable_list->GetVariableAtIndex(i));
                    if (variable_sp)
                    {
                        bool add_variable = false;
                        switch (variable_sp->GetScope())
                        {
                        case eValueTypeVariableGlobal:
                        case eValueTypeVariableStatic:
                            add_variable = statics;
                            break;

                        case eValueTypeVariableArgument:
                            add_variable = arguments;
                            break;

                        case eValueTypeVariableLocal:
                            add_variable = locals;
                            break;

                        default:
                            break;
                        }
                        if (add_variable)
                        {
                            if (in_scope_only && !variable_sp->IsInScope(m_opaque_sp.get()))
                                continue;

                            value_list.Append(m_opaque_sp->GetValueObjectForFrameVariable (variable_sp));
                        }
                    }
                }
            }
        }        
    }

    if (log)
    {
        log->Printf ("SBFrame::GetVariables (this.sp=%p,...) => SBValueList (this.ap = %p)", m_opaque_sp.get(),
                     value_list.get());
        //uint32_t num_vars = value_list.GetSize();
        //for (uint32_t i = 0; i < num_vars; ++i)
        //{
        //    SBValue value = value_list.GetValueAtIndex (i);
        //    log->Printf ("  %s : %s", value.GetName(), value.GetObjectDescription (*this));
        //}
    }

    return value_list;
}

lldb::SBValueList
SBFrame::GetRegisters ()
{
    Log *log = lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_API);

    //if (log)
    //    log->Printf ("SBFrame::GetRegisters ()");

    SBValueList value_list;
    if (m_opaque_sp)
    {
        RegisterContext *reg_ctx = m_opaque_sp->GetRegisterContext();
        if (reg_ctx)
        {
            const uint32_t num_sets = reg_ctx->GetRegisterSetCount();
            for (uint32_t set_idx = 0; set_idx < num_sets; ++set_idx)
            {
                value_list.Append(ValueObjectSP (new ValueObjectRegisterSet (NULL, reg_ctx, set_idx)));
            }
        }
    }

    if (log)
    {
        log->Printf ("SBFrame::Registers (this.sp=%p) => SBValueList (this.ap = %p)", m_opaque_sp.get(),
                     value_list.get() );
        //uint32_t num_vars = value_list.GetSize();
        //for (uint32_t i = 0; i < num_vars; ++i)
        //{
        //    SBValue value = value_list.GetValueAtIndex (i);
        //    log->Printf ("  %s : %s", value.GetName(), value.GetObjectDescription (*this));
        //}
    }

    return value_list;
}

bool
SBFrame::GetDescription (SBStream &description)
{
    if (m_opaque_sp)
    {
        SBLineEntry line_entry = GetLineEntry ();
        SBFileSpec file_spec = line_entry.GetFileSpec ();
        uint32_t line = line_entry.GetLine ();
        description.Printf("SBFrame: idx = %u ('%s', %s, line %d)", m_opaque_sp->GetFrameIndex(), 
                           GetFunction().GetName(), file_spec.GetFilename(), line);
    }
    else
        description.Printf ("No value");

    return true;
}

lldb::SBValue
SBFrame::EvaluateExpression (const char *expr)
{
    lldb::SBValue expr_result_value;
    if (m_opaque_sp)
    {
        ExecutionContext exe_ctx;
        m_opaque_sp->CalculateExecutionContext (exe_ctx);
        
        const char *prefix = NULL;
        
        if (exe_ctx.target)
            prefix = exe_ctx.target->GetExpressionPrefixContentsAsCString();
        
        *expr_result_value = ClangUserExpression::Evaluate (exe_ctx, expr, prefix);
    }
    return expr_result_value;
}
