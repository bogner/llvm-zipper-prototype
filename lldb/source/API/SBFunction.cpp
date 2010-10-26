//===-- SBFunction.cpp ------------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "lldb/API/SBFunction.h"
#include "lldb/API/SBProcess.h"
#include "lldb/API/SBStream.h"
#include "lldb/Core/Disassembler.h"
#include "lldb/Core/Log.h"
#include "lldb/Core/Module.h"
#include "lldb/Symbol/CompileUnit.h"
#include "lldb/Symbol/Function.h"
#include "lldb/Symbol/Type.h"
#include "lldb/Target/ExecutionContext.h"
#include "lldb/Target/Target.h"

using namespace lldb;
using namespace lldb_private;

SBFunction::SBFunction () :
    m_opaque_ptr (NULL)
{
}

SBFunction::SBFunction (lldb_private::Function *lldb_object_ptr) :
    m_opaque_ptr (lldb_object_ptr)
{
    Log *log = lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_API);

    if (log)
    {
        SBStream sstr;
        GetDescription (sstr);
        log->Printf ("SBFunction::SBFunction (lldb_object_ptr=%p) => this.obj = %p ('%s')", lldb_object_ptr, 
                     m_opaque_ptr, sstr.GetData());
                     
    }
}

SBFunction::~SBFunction ()
{
    m_opaque_ptr = NULL;
}

bool
SBFunction::IsValid () const
{
    return m_opaque_ptr != NULL;
}

const char *
SBFunction::GetName() const
{
    Log *log = lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_API);

    //if (log)
    //    log->Printf ("SBFunction::GetName ()"); 

    if (m_opaque_ptr)
    {
        if (log)
            log->Printf ("SBFunction::GetName (this.obj=%p) => '%s'", m_opaque_ptr,
                         m_opaque_ptr->GetMangled().GetName().AsCString());
        return m_opaque_ptr->GetMangled().GetName().AsCString();
    }

    if (log)
        log->Printf ("SBFunction::GetName (this.obj=%p) => NULL", m_opaque_ptr);
    return NULL;
}

const char *
SBFunction::GetMangledName () const
{
    if (m_opaque_ptr)
        return m_opaque_ptr->GetMangled().GetMangledName().AsCString();
    return NULL;
}

bool
SBFunction::operator == (const SBFunction &rhs) const
{
    return m_opaque_ptr == rhs.m_opaque_ptr;
}

bool
SBFunction::operator != (const SBFunction &rhs) const
{
    return m_opaque_ptr != rhs.m_opaque_ptr;
}

bool
SBFunction::GetDescription (SBStream &s)
{
    if (m_opaque_ptr)
    {
        s.Printf ("SBFunction: id = 0x%8.8x, name = %s", 
                            m_opaque_ptr->GetID(),
                            m_opaque_ptr->GetName().AsCString());
        Type *func_type = m_opaque_ptr->GetType();
        if (func_type)
            s.Printf(", type = %s", func_type->GetName().AsCString());
        return true;
    }
    s.Printf ("No value");
    return false;
}

SBInstructionList
SBFunction::GetInstructions (SBTarget target)
{
    SBInstructionList sb_instructions;
    if (m_opaque_ptr)
    {
        ExecutionContext exe_ctx;
        if (target.IsValid())
        {
            target->CalculateExecutionContext (exe_ctx);
            exe_ctx.process = target->GetProcessSP().get();
        }
        Module *module = m_opaque_ptr->GetAddressRange().GetBaseAddress().GetModule();
        if (module)
        {
            sb_instructions.SetDisassembler (Disassembler::DisassembleRange (module->GetArchitecture(),
                                                                             exe_ctx,
                                                                             m_opaque_ptr->GetAddressRange()));
        }
    }
    return sb_instructions;
}

lldb_private::Function *
SBFunction::get ()
{
    return m_opaque_ptr;
}

