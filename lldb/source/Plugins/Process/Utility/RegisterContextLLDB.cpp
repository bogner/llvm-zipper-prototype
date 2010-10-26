//===-- RegisterContextLLDB.cpp --------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "lldb/lldb-private.h"
#include "RegisterContextLLDB.h"
#include "lldb/Target/Thread.h"
#include "lldb/Symbol/Function.h"
#include "lldb/Symbol/SymbolContext.h"
#include "lldb/Symbol/Symbol.h"
#include "lldb/Core/Address.h"
#include "lldb/Core/AddressRange.h"
#include "lldb/Target/Target.h"
#include "lldb/Target/Process.h"
#include "lldb/Utility/ArchDefaultUnwindPlan.h"
#include "lldb/Symbol/FuncUnwinders.h"
#include "lldb/Core/DataBufferHeap.h"
#include "lldb/Utility/ArchVolatileRegs.h"
#include "lldb/Core/Log.h"

using namespace lldb;
using namespace lldb_private;


RegisterContextLLDB::RegisterContextLLDB (Thread& thread, 
                                          const RegisterContextSP &next_frame,
                                          SymbolContext& sym_ctx,
                                          int frame_number) :
    RegisterContext (thread), m_thread(thread), m_next_frame(next_frame), 
    m_zeroth_frame(false), m_sym_ctx(sym_ctx), m_all_registers_available(false), m_registers(),
    m_cfa (LLDB_INVALID_ADDRESS), m_start_pc (), m_frame_number (frame_number)
{
    m_base_reg_ctx = m_thread.GetRegisterContext();
    if (m_next_frame.get() == NULL)
    {
        InitializeZerothFrame ();
    }
    else
    {
        InitializeNonZerothFrame ();
    }
}

// Initialize a RegisterContextLLDB which is the first frame of a stack -- the zeroth frame or currently
// executing frame.

void
RegisterContextLLDB::InitializeZerothFrame()
{
    m_zeroth_frame = true;
    StackFrameSP frame_sp (m_thread.GetStackFrameAtIndex (0));
    if (m_base_reg_ctx == NULL)
    {
        m_frame_type = eNotAValidFrame;
        return;
    }
    m_sym_ctx = frame_sp->GetSymbolContext (eSymbolContextEverything);
    const AddressRange *addr_range_ptr;
    if (m_sym_ctx.function)
        addr_range_ptr = &m_sym_ctx.function->GetAddressRange();
    else if (m_sym_ctx.symbol)
        addr_range_ptr = m_sym_ctx.symbol->GetAddressRangePtr();

    Address current_pc = frame_sp->GetFrameCodeAddress();

    static ConstString sigtramp_name ("_sigtramp");
    if ((m_sym_ctx.function && m_sym_ctx.function->GetMangled().GetMangledName() == sigtramp_name)
        || (m_sym_ctx.symbol && m_sym_ctx.symbol->GetMangled().GetMangledName() == sigtramp_name))
    {
        m_frame_type = eSigtrampFrame;
    }
    else
    {
        // FIXME:  Detect eDebuggerFrame here.
        m_frame_type = eNormalFrame;
    }

    // If we were able to find a symbol/function, set addr_range_ptr to the bounds of that symbol/function.
    // else treat the current pc value as the start_pc and record no offset.
    if (addr_range_ptr)
    {
        m_start_pc = addr_range_ptr->GetBaseAddress();
        m_current_offset = frame_sp->GetFrameCodeAddress().GetOffset() - m_start_pc.GetOffset();
    }
    else
    {
        m_start_pc = current_pc;
        m_current_offset = -1;
    }

    // We've set m_frame_type, m_zeroth_frame, and m_sym_ctx before this call.
    // This call sets the m_all_registers_available, m_fast_unwind_plan, and m_full_unwind_plan member variables.
    GetUnwindPlansForFrame (current_pc);

    const UnwindPlan::Row *active_row = NULL;
    int cfa_offset = 0;
    int row_register_kind;
    if (m_full_unwind_plan && m_full_unwind_plan->PlanValidAtAddress (current_pc))
    {
        active_row = m_full_unwind_plan->GetRowForFunctionOffset (m_current_offset);
        row_register_kind = m_full_unwind_plan->GetRegisterKind ();
    }

    if (active_row == NULL)
    {
        m_frame_type = eNotAValidFrame;
        return;
    }

    addr_t cfa_regval;
    if (!ReadGPRValue (row_register_kind, active_row->GetCFARegister(), cfa_regval))
    {
        m_frame_type = eNotAValidFrame;
        return;
    }
    else
    {
    }
    cfa_offset = active_row->GetCFAOffset ();

    m_cfa = cfa_regval + cfa_offset;

    Log *log = GetLogIfAllCategoriesSet (LIBLLDB_LOG_UNWIND);

    // A couple of sanity checks..
    if (m_cfa == (addr_t) -1 || m_cfa == 0 || m_cfa == 1)
    {
        if (log)
        {   
            log->Printf("%*sFrame %d could not find a valid cfa address",
                        m_frame_number, "", m_frame_number);
        }
        m_frame_type = eNotAValidFrame;
        return;
    }

    if (log)
    {
        log->Printf("%*sFrame %d initialized frame current pc is 0x%llx cfa is 0x%llx", 
                    m_frame_number, "", m_frame_number,
                    (uint64_t) m_cfa, (uint64_t) current_pc.GetLoadAddress (&m_thread.GetProcess().GetTarget()));
    }
}

// Initialize a RegisterContextLLDB for the non-zeroth frame -- rely on the RegisterContextLLDB "below" it
// to provide things like its current pc value.

void
RegisterContextLLDB::InitializeNonZerothFrame()
{
    Log *log = GetLogIfAllCategoriesSet (LIBLLDB_LOG_UNWIND);
    if (m_next_frame.get() == NULL)
    {
        m_frame_type = eNotAValidFrame;
        return;
    }
    if (!((RegisterContextLLDB*)m_next_frame.get())->IsValid())
    {
        m_frame_type = eNotAValidFrame;
        return;
    }
    if (m_base_reg_ctx == NULL)
    {
        m_frame_type = eNotAValidFrame;
        return;
    }

    m_zeroth_frame = false;
    
    addr_t pc;
    if (!ReadGPRValue (eRegisterKindGeneric, LLDB_REGNUM_GENERIC_PC, pc))
    {
        if (log)
        {
            log->Printf("%*sFrame %d could not get pc value",
                        m_frame_number, "", m_frame_number);
        }
        m_frame_type = eNotAValidFrame;
        return;
    }
    Address current_pc;
    m_thread.GetProcess().GetTarget().GetSectionLoadList().ResolveLoadAddress (pc, current_pc);

    // If we don't have a Module for some reason, we're not going to find symbol/function information - just
    // stick in some reasonable defaults and hope we can unwind past this frame.
    if (!current_pc.IsValid() || current_pc.GetModule() == NULL)
    {
        if (log)
        {
            log->Printf("%*sFrame %d using architectural default unwind method",
                        m_frame_number, "", m_frame_number);
        }
        ArchSpec arch = m_thread.GetProcess().GetTarget().GetArchitecture ();
        ArchDefaultUnwindPlan *arch_default = ArchDefaultUnwindPlan::FindPlugin (arch);
        if (arch_default)
        {
            m_fast_unwind_plan = NULL;
            m_full_unwind_plan = arch_default->GetArchDefaultUnwindPlan (m_thread, current_pc);
            m_frame_type = eNormalFrame;
            m_all_registers_available = false;
            m_current_offset = -1;
            addr_t cfa_regval;
            int row_register_kind = m_full_unwind_plan->GetRegisterKind ();
            uint32_t cfa_regnum = m_full_unwind_plan->GetRowForFunctionOffset(0)->GetCFARegister();
            int cfa_offset = m_full_unwind_plan->GetRowForFunctionOffset(0)->GetCFAOffset();
            if (!ReadGPRValue (row_register_kind, cfa_regnum, cfa_regval))
            {
                if (log)
                {
                    log->Printf("%*sFrame %d failed to get cfa value",
                                m_frame_number, "", m_frame_number);
                }
                m_frame_type = eNormalFrame;
                return;
            }
            m_cfa = cfa_regval + cfa_offset;
            if (log)
            {
                log->Printf("%*sFrame %d initialized frame current pc is 0x%llx cfa is 0x%llx",
                            m_frame_number, "", m_frame_number,
                            (uint64_t) m_cfa, (uint64_t) current_pc.GetLoadAddress (&m_thread.GetProcess().GetTarget()));
            }
            return;
        }
        m_frame_type = eNotAValidFrame;
        return;
    }

    // set up our m_sym_ctx SymbolContext
    current_pc.GetModule()->ResolveSymbolContextForAddress (current_pc, eSymbolContextFunction | eSymbolContextSymbol, m_sym_ctx);

    const AddressRange *addr_range_ptr;
    if (m_sym_ctx.function)
        addr_range_ptr = &m_sym_ctx.function->GetAddressRange();
    else if (m_sym_ctx.symbol)
        addr_range_ptr = m_sym_ctx.symbol->GetAddressRangePtr();

    static ConstString sigtramp_name ("_sigtramp");
    if ((m_sym_ctx.function && m_sym_ctx.function->GetMangled().GetMangledName() == sigtramp_name)
        || (m_sym_ctx.symbol && m_sym_ctx.symbol->GetMangled().GetMangledName() == sigtramp_name))
    {
        m_frame_type = eSigtrampFrame;
    }
    else
    {
        // FIXME:  Detect eDebuggerFrame here.
        m_frame_type = eNormalFrame;
    }

    // If we were able to find a symbol/function, set addr_range_ptr to the bounds of that symbol/function.
    // else treat the current pc value as the start_pc and record no offset.
    if (addr_range_ptr)
    {
        m_start_pc = addr_range_ptr->GetBaseAddress();
        m_current_offset = current_pc.GetOffset() - m_start_pc.GetOffset();
    }
    else
    {
        m_start_pc = current_pc;
        m_current_offset = -1;
    }

    // We've set m_frame_type, m_zeroth_frame, and m_sym_ctx before this call.
    // This call sets the m_all_registers_available, m_fast_unwind_plan, and m_full_unwind_plan member variables.
    GetUnwindPlansForFrame (current_pc);

    const UnwindPlan::Row *active_row = NULL;
    int cfa_offset = 0;
    int row_register_kind;
    if (m_fast_unwind_plan && m_fast_unwind_plan->PlanValidAtAddress (current_pc))
    {
        active_row = m_fast_unwind_plan->GetRowForFunctionOffset (m_current_offset);
        row_register_kind = m_fast_unwind_plan->GetRegisterKind ();
    }
    else if (m_full_unwind_plan && m_full_unwind_plan->PlanValidAtAddress (current_pc))
    {
        active_row = m_full_unwind_plan->GetRowForFunctionOffset (m_current_offset);
        row_register_kind = m_full_unwind_plan->GetRegisterKind ();
    }

    if (active_row == NULL)
    {
        m_frame_type = eNotAValidFrame;
        return;
    }

    addr_t cfa_regval;
    if (!ReadGPRValue (row_register_kind, active_row->GetCFARegister(), cfa_regval))
    {
        if (log)
        {
            log->Printf("%*sFrame %d failed to get cfa reg %d/%d",
                        m_frame_number, "", m_frame_number,
                        row_register_kind, active_row->GetCFARegister());
        }
        m_frame_type = eNotAValidFrame;
        return;
    }
    cfa_offset = active_row->GetCFAOffset ();

    m_cfa = cfa_regval + cfa_offset;

    // A couple of sanity checks..
    if (m_cfa == (addr_t) -1 || m_cfa == 0 || m_cfa == 1)
    { 
        if (log)
        {
            log->Printf("%*sFrame %d could not find a valid cfa address",
                        m_frame_number, "", m_frame_number);
        }
        m_frame_type = eNotAValidFrame;
        return;
    }

    if (log)
    {
        log->Printf("%*sFrame %d initialized frame current pc is 0x%llx cfa is 0x%llx", 
                    m_frame_number, "", m_frame_number,
                    (uint64_t) m_cfa, (uint64_t) current_pc.GetLoadAddress (&m_thread.GetProcess().GetTarget()));
    }
}




// On entry to this method, 
//
//   1. m_frame_type should already be set to eSigtrampFrame/eDebuggerFrame 
//      if either of those are correct, and
//   2. m_zeroth_frame should be set to true if this is frame 0 and
//   3. m_sym_ctx should already be filled in.
//
// On exit this function will have set
//
//   a. m_all_registers_available  (true if we can provide any requested register, false if only a subset are provided)
//   b. m_fast_unwind_plan (fast unwind plan that walks the stack while filling in only minimal registers, may be NULL)
//   c. m_full_unwind_plan (full unwind plan that can provide all registers possible, will *not* be NULL)
//
// The argument current_pc should be the current pc value in the function.  

void
RegisterContextLLDB::GetUnwindPlansForFrame (Address current_pc)
{
    UnwindPlan *arch_default_up = NULL;
    ArchSpec arch = m_thread.GetProcess().GetTarget().GetArchitecture ();
    ArchDefaultUnwindPlan *arch_default = ArchDefaultUnwindPlan::FindPlugin (arch);
    if (arch_default)
    {
        arch_default_up = arch_default->GetArchDefaultUnwindPlan (m_thread, current_pc);
    }

    bool behaves_like_zeroth_frame = false;

    if (m_zeroth_frame)
    {
        behaves_like_zeroth_frame = true;
    }
    if (m_next_frame.get() && ((RegisterContextLLDB*) m_next_frame.get())->m_frame_type == eSigtrampFrame)
    {
        behaves_like_zeroth_frame = true;
    }
    if (m_next_frame.get() && ((RegisterContextLLDB*) m_next_frame.get())->m_frame_type == eDebuggerFrame)
    {
        behaves_like_zeroth_frame = true;
    }

    if (behaves_like_zeroth_frame)
    {
        m_all_registers_available = true;
    }
    else
    {
//        If we need to implement gdb's decrement-pc-value-by-one-before-function-check macro, it would be here.
//        current_pc.SetOffset (current_pc.GetOffset() - 1);
        m_all_registers_available = false;
    }

    // No Module for the current pc, try using the architecture default unwind.
    if (current_pc.GetModule() == NULL || current_pc.GetModule()->GetObjectFile() == NULL)
    {
        m_fast_unwind_plan = NULL;
        m_full_unwind_plan = arch_default_up;
        m_frame_type = eNormalFrame;
        return;
    }

    FuncUnwindersSP fu;
    if (current_pc.GetModule() && current_pc.GetModule()->GetObjectFile())
    {
       fu = current_pc.GetModule()->GetObjectFile()->GetUnwindTable().GetFuncUnwindersContainingAddress (current_pc, m_sym_ctx);
    }

    // No FuncUnwinders available for this pc, try using architectural default unwind.
    if (fu.get() == NULL)
    {
        m_fast_unwind_plan = NULL;
        m_full_unwind_plan = arch_default_up;
        m_frame_type = eNormalFrame;
        return;
    }

    // If we're in _sigtramp(), unwinding past this frame requires special knowledge.  On Mac OS X this knowledge
    // is properly encoded in the eh_frame section, so prefer that if available.
    if (m_frame_type == eSigtrampFrame)
    {
        m_fast_unwind_plan = NULL;
        UnwindPlan *up = fu->GetUnwindPlanAtCallSite ();
        if (up->PlanValidAtAddress (current_pc))
        {
            m_fast_unwind_plan = NULL;
            m_full_unwind_plan = up;
            return;
        }
    }


    UnwindPlan *fast, *callsite, *noncallsite;
    fast = callsite = noncallsite = NULL;

    if (fu->GetUnwindPlanFastUnwind (m_thread) 
        && fu->GetUnwindPlanFastUnwind (m_thread)->PlanValidAtAddress (current_pc))
    {
        fast = fu->GetUnwindPlanFastUnwind (m_thread);
    }

    // Typically this is the unwind created by inspecting the assembly language instructions
    if (fu->GetUnwindPlanAtNonCallSite (m_thread) 
        && fu->GetUnwindPlanAtNonCallSite (m_thread)->PlanValidAtAddress (current_pc))
    {
        noncallsite = fu->GetUnwindPlanAtNonCallSite (m_thread);
    }


    // Typically this is unwind info from an eh_frame section intended for exception handling; only valid at call sites
    if (fu->GetUnwindPlanAtCallSite () 
        && fu->GetUnwindPlanAtCallSite ()->PlanValidAtAddress (current_pc))
    {
        callsite = fu->GetUnwindPlanAtCallSite ();
    }

    m_fast_unwind_plan = NULL;
    m_full_unwind_plan = NULL;

    if (fast)
    {
        m_fast_unwind_plan = fast;
    }

    if (behaves_like_zeroth_frame && noncallsite)
    {
        m_full_unwind_plan = noncallsite;
    }
    else 
    {
        if (callsite)
        {
            m_full_unwind_plan = callsite;
        }
        else
        {
            m_full_unwind_plan = noncallsite;
        }
    }

    if (m_full_unwind_plan == NULL)
    {
        m_full_unwind_plan = arch_default_up;
    }

    Log *log = GetLogIfAllCategoriesSet (LIBLLDB_LOG_UNWIND);
    if (log && IsLogVerbose())
    {
        const char *has_fast = "";
        if (m_fast_unwind_plan)
            has_fast = ", and has a fast UnwindPlan";
        log->Printf("%*sFrame %d frame uses %s for full UnwindPlan%s",
                    m_frame_number, "", m_frame_number,
                    m_full_unwind_plan->GetSourceName().GetCString(), has_fast);
    }

    return;
}

void
RegisterContextLLDB::Invalidate ()
{
    m_frame_type = eNotAValidFrame;
}

size_t
RegisterContextLLDB::GetRegisterCount ()
{
    return m_base_reg_ctx->GetRegisterCount();
}

const RegisterInfo *
RegisterContextLLDB::GetRegisterInfoAtIndex (uint32_t reg)
{
    return m_base_reg_ctx->GetRegisterInfoAtIndex (reg);
}

size_t
RegisterContextLLDB::GetRegisterSetCount ()
{
    return m_base_reg_ctx->GetRegisterSetCount ();
}

const RegisterSet *
RegisterContextLLDB::GetRegisterSet (uint32_t reg_set)
{
    return m_base_reg_ctx->GetRegisterSet (reg_set);
}

uint32_t
RegisterContextLLDB::ConvertRegisterKindToRegisterNumber (uint32_t kind, uint32_t num)
{
    return m_base_reg_ctx->ConvertRegisterKindToRegisterNumber (kind, num);
}

bool
RegisterContextLLDB::ReadRegisterBytesFromRegisterLocation (uint32_t regnum, RegisterLocation regloc, DataExtractor &data)
{
    if (!IsValid())
        return false;

    if (regloc.type == eRegisterInRegister)
    {
        data.SetAddressByteSize (m_thread.GetProcess().GetAddressByteSize());
        data.SetByteOrder (m_thread.GetProcess().GetByteOrder());
        if (m_next_frame.get() == NULL)
        {
            return m_base_reg_ctx->ReadRegisterBytes (regloc.location.register_number, data);
        }
        else
        {
            return m_next_frame->ReadRegisterBytes (regloc.location.register_number, data);
        }
    }
    if (regloc.type == eRegisterNotSaved)
    {
        return false;
    }
    if (regloc.type == eRegisterSavedAtHostMemoryLocation)
    {
        assert ("FIXME debugger inferior function call unwind");
    }
    if (regloc.type != eRegisterSavedAtMemoryLocation)
    {
        assert ("Unknown RegisterLocation type.");
    }

    const RegisterInfo *reg_info = m_base_reg_ctx->GetRegisterInfoAtIndex (regnum);
    DataBufferSP data_sp (new DataBufferHeap (reg_info->byte_size, 0));
    data.SetData (data_sp, 0, reg_info->byte_size);
    data.SetAddressByteSize (m_thread.GetProcess().GetAddressByteSize());

    if (regloc.type == eRegisterValueInferred)
    {
        data.SetByteOrder (eByteOrderHost);
        switch (reg_info->byte_size)
        {
            case 1:
            {
                uint8_t val = regloc.location.register_value;
                memcpy (data_sp->GetBytes(), &val, sizeof (val));
                data.SetByteOrder (eByteOrderHost);
                return true;
            }
            case 2:
            {
                uint16_t val = regloc.location.register_value;
                memcpy (data_sp->GetBytes(), &val, sizeof (val));
                data.SetByteOrder (eByteOrderHost);
                return true;
            }
            case 4:
            {
                uint32_t val = regloc.location.register_value;
                memcpy (data_sp->GetBytes(), &val, sizeof (val));
                data.SetByteOrder (eByteOrderHost);
                return true;
            }
            case 8:
            {
                uint64_t val = regloc.location.register_value;
                memcpy (data_sp->GetBytes(), &val, sizeof (val));
                data.SetByteOrder (eByteOrderHost);
                return true;
            }
        }
        return false;
    }

    assert (regloc.type == eRegisterSavedAtMemoryLocation);
    Error error;
    data.SetByteOrder (m_thread.GetProcess().GetByteOrder());
    if (!m_thread.GetProcess().ReadMemory (regloc.location.target_memory_location, data_sp->GetBytes(), reg_info->byte_size, error))
        return false;
    return true;
}

bool
RegisterContextLLDB::IsValid () const
{
    return m_frame_type != eNotAValidFrame;
}

// Answer the question: Where did THIS frame save the CALLER frame ("previous" frame)'s register value?

bool
RegisterContextLLDB::SavedLocationForRegister (uint32_t lldb_regnum, RegisterLocation &regloc)
{
    Log *log = GetLogIfAllCategoriesSet (LIBLLDB_LOG_UNWIND);

    // Have we already found this register location?
    std::map<uint32_t, RegisterLocation>::const_iterator iterator;
    if (m_registers.size() > 0)
    {
        iterator = m_registers.find (lldb_regnum);
        if (iterator != m_registers.end())
        {
            regloc = iterator->second;
            return true;
        }
    }

    // Are we looking for the CALLER's stack pointer?  The stack pointer is defined to be the same as THIS frame's
    // CFA so just return the CFA value.  This is true on x86-32/x86-64 at least.
    uint32_t sp_regnum;
    if (m_base_reg_ctx->ConvertBetweenRegisterKinds (eRegisterKindGeneric, LLDB_REGNUM_GENERIC_SP, eRegisterKindLLDB, sp_regnum)
        && sp_regnum == lldb_regnum)
    {
        // make sure we won't lose precision copying an addr_t (m_cfa) into a uint64_t (.register_value)
        assert (sizeof (addr_t) <= sizeof (uint64_t));
        regloc.type = eRegisterValueInferred;
        regloc.location.register_value = m_cfa;
        m_registers[lldb_regnum] = regloc;
        return true;
    }

    // Look through the available UnwindPlans for the register location.

    UnwindPlan::Row::RegisterLocation unwindplan_regloc;
    bool have_unwindplan_regloc = false;
    if (m_fast_unwind_plan)
    {
        const UnwindPlan::Row *active_row = m_fast_unwind_plan->GetRowForFunctionOffset (m_current_offset);
        uint32_t row_regnum;
        if (!m_base_reg_ctx->ConvertBetweenRegisterKinds (eRegisterKindLLDB, lldb_regnum, m_fast_unwind_plan->GetRegisterKind(), row_regnum))
        {
            if (log)
            {
                log->Printf("%*sFrame %d could not supply caller's reg %d location",
                            m_frame_number, "", m_frame_number,
                            lldb_regnum);
            }
            return false;
        }
        if (active_row->GetRegisterInfo (row_regnum, unwindplan_regloc))
        {
            if (log)
            {
                log->Printf("%*sFrame %d supplying caller's saved reg %d's location using FastUnwindPlan",
                            m_frame_number, "", m_frame_number,
                            lldb_regnum);
            }
            have_unwindplan_regloc = true;
        }
    }
    else if (m_full_unwind_plan)
    {
        const UnwindPlan::Row *active_row = m_full_unwind_plan->GetRowForFunctionOffset (m_current_offset);
        uint32_t row_regnum;
        if (!m_base_reg_ctx->ConvertBetweenRegisterKinds (eRegisterKindLLDB, lldb_regnum, m_full_unwind_plan->GetRegisterKind(), row_regnum))
        {
            if (log)
            {
                log->Printf("%*sFrame %d could not supply caller's reg %d location",
                            m_frame_number, "", m_frame_number,
                            lldb_regnum);
            }
            return false;
        }

        if (active_row->GetRegisterInfo (row_regnum, unwindplan_regloc))
        {
            have_unwindplan_regloc = true;
            if (log && IsLogVerbose ())
            {                
                log->Printf("%*sFrame %d supplying caller's saved reg %d's location using %s UnwindPlan",
                            m_frame_number, "", m_frame_number,
                            lldb_regnum, m_full_unwind_plan->GetSourceName().GetCString());
            }
        }
    }
    if (have_unwindplan_regloc == false)
    {
        // If a volatile register is being requested, we don't want to forward m_next_frame's register contents 
        // up the stack -- the register is not retrievable at this frame.
        ArchSpec arch = m_thread.GetProcess().GetTarget().GetArchitecture ();
        ArchVolatileRegs *volatile_regs = ArchVolatileRegs::FindPlugin (arch);
        if (volatile_regs && volatile_regs->RegisterIsVolatile (m_thread, lldb_regnum))
        {
            if (log)
            {
                log->Printf("%*sFrame %d did not supply reg location for %d because it is volatile",
                            m_frame_number, "", m_frame_number,
                            lldb_regnum);
            }
            return false;
        }  

        if (m_next_frame.get())
        {
            return ((RegisterContextLLDB*)m_next_frame.get())->SavedLocationForRegister (lldb_regnum, regloc);
        }
        else
        {
            // This is frame 0 - we should return the actual live register context value
            RegisterLocation new_regloc;
            new_regloc.type = eRegisterInRegister;
            new_regloc.location.register_number = lldb_regnum;
            m_registers[lldb_regnum] = new_regloc;
            regloc = new_regloc;
            return true;
        }
        if (log)
        {
            log->Printf("%*sFrame %d could not supply caller's reg %d location",
                        m_frame_number, "", m_frame_number,
                        lldb_regnum);
        }
        return false;
    }

    // unwindplan_regloc has valid contents about where to retrieve the register
    if (unwindplan_regloc.IsUnspecified())
    {
        RegisterLocation new_regloc;
        new_regloc.type = eRegisterNotSaved;
        m_registers[lldb_regnum] = new_regloc;
        if (log)
        {
            log->Printf("%*sFrame %d could not supply caller's reg %d location",
                        m_frame_number, "", m_frame_number,
                        lldb_regnum);
        }
        return false;
    }

    if (unwindplan_regloc.IsSame())
    {
        if (m_next_frame.get())
        {
            return ((RegisterContextLLDB*)m_next_frame.get())->SavedLocationForRegister (lldb_regnum, regloc);
        }
        else
        {
            if (log)
            {
                log->Printf("%*sFrame %d could not supply caller's reg %d location",
                            m_frame_number, "", m_frame_number,
                            lldb_regnum);
            }
            return false;
        }
    }

    if (unwindplan_regloc.IsCFAPlusOffset())
    {
        int offset = unwindplan_regloc.GetOffset();
        regloc.type = eRegisterValueInferred;
        regloc.location.register_value = m_cfa + offset;
        m_registers[lldb_regnum] = regloc;
        return true;
    }

    if (unwindplan_regloc.IsAtCFAPlusOffset())
    {
        int offset = unwindplan_regloc.GetOffset();
        regloc.type = eRegisterSavedAtMemoryLocation;
        regloc.location.target_memory_location = m_cfa + offset;
        m_registers[lldb_regnum] = regloc;
        return true;
    }

    if (unwindplan_regloc.IsInOtherRegister())
    {
        uint32_t unwindplan_regnum = unwindplan_regloc.GetRegisterNumber();
        uint32_t row_regnum_in_lldb;
        if (!m_base_reg_ctx->ConvertBetweenRegisterKinds (m_full_unwind_plan->GetRegisterKind(), unwindplan_regnum, eRegisterKindLLDB, row_regnum_in_lldb))
        {
            if (log)
            {
                log->Printf("%*sFrame %d could not supply caller's reg %d location",
                            m_frame_number, "", m_frame_number,
                            lldb_regnum);
            }
            return false;
        }
        regloc.type = eRegisterInRegister;
        regloc.location.register_number = row_regnum_in_lldb;
        m_registers[lldb_regnum] = regloc;
        return true;
    }

    if (log)
    {
        log->Printf("%*sFrame %d could not supply caller's reg %d location",
                    m_frame_number, "", m_frame_number,
                    lldb_regnum);
    }

    assert ("UnwindPlan::Row types atDWARFExpression and isDWARFExpression are unsupported.");
    return false;
}

// Retrieve a general purpose register value for THIS from, as saved by the NEXT frame, i.e. the frame that
// this frame called.  e.g.
//
//  foo () { }
//  bar () { foo (); }
//  main () { bar (); }
//
//  stopped in foo() so
//     frame 0 - foo
//     frame 1 - bar
//     frame 2 - main
//  and this RegisterContext is for frame 1 (bar) - if we want to get the pc value for frame 1, we need to ask
//  where frame 0 (the "next" frame) saved that and retrieve the value.

// Assumes m_base_reg_ctx has been set
bool
RegisterContextLLDB::ReadGPRValue (int register_kind, uint32_t regnum, addr_t &value)
{
    if (!IsValid())
        return false;

    uint32_t lldb_regnum;
    if (register_kind == eRegisterKindLLDB)
    {
        lldb_regnum = regnum;
    }
    else if (!m_base_reg_ctx->ConvertBetweenRegisterKinds (register_kind, regnum, eRegisterKindLLDB, lldb_regnum))
    {
        return false;
    }

    uint32_t offset = 0;
    DataExtractor data;
    data.SetAddressByteSize (m_thread.GetProcess().GetAddressByteSize());
    data.SetByteOrder (m_thread.GetProcess().GetByteOrder());

    // if this is frame 0 (currently executing frame), get the requested reg contents from the actual thread registers
    if (m_next_frame.get() == NULL)
    {
        if (m_base_reg_ctx->ReadRegisterBytes (lldb_regnum, data))
        {
            data.SetAddressByteSize (m_thread.GetProcess().GetAddressByteSize());
            value = data.GetAddress (&offset);
            return true;
        }
        else
        {
            return false;
        }
    }

    RegisterLocation regloc;
    if (!((RegisterContextLLDB*)m_next_frame.get())->SavedLocationForRegister (lldb_regnum, regloc))
    {
        return false;
    }
    if (!ReadRegisterBytesFromRegisterLocation (lldb_regnum, regloc, data))
    {
        return false;
    }
    data.SetAddressByteSize (m_thread.GetProcess().GetAddressByteSize());
    value = data.GetAddress (&offset);
    return true;
}

// Find the value of a register in THIS frame

bool
RegisterContextLLDB::ReadRegisterBytes (uint32_t lldb_reg, DataExtractor& data)
{
    Log *log = GetLogIfAllCategoriesSet (LIBLLDB_LOG_UNWIND);
    if (!IsValid())
        return false;

    if (log && IsLogVerbose ())
    {
        log->Printf("%*sFrame %d looking for register saved location for reg %d",
                    m_frame_number, "", m_frame_number,
                    lldb_reg);
    }

    // If this is the 0th frame, hand this over to the live register context
    if (m_next_frame.get() == NULL)
    {
        if (log)
        {
            log->Printf("%*sFrame %d passing along to the live register context for reg %d",
                        m_frame_number, "", m_frame_number,
                        lldb_reg);
        }
        return m_base_reg_ctx->ReadRegisterBytes (lldb_reg, data);
    }

    RegisterLocation regloc;
    // Find out where the NEXT frame saved THIS frame's register contents
    if (!((RegisterContextLLDB*)m_next_frame.get())->SavedLocationForRegister (lldb_reg, regloc))
        return false;

    return ReadRegisterBytesFromRegisterLocation (lldb_reg, regloc, data);
}

bool
RegisterContextLLDB::ReadAllRegisterValues (lldb::DataBufferSP &data_sp)
{
    assert ("not yet implemented");  // FIXME
    return false;
}

bool
RegisterContextLLDB::WriteRegisterBytes (uint32_t reg, DataExtractor &data, uint32_t data_offset)
{
    assert ("not yet implemented");  // FIXME
    return false;
}

bool
RegisterContextLLDB::WriteAllRegisterValues (const lldb::DataBufferSP& data_sp)
{
    assert ("not yet implemented");  // FIXME
    return false;
}

// Retrieve the pc value for THIS from

bool
RegisterContextLLDB::GetCFA (addr_t& cfa)
{
    if (!IsValid())
    {
        return false;
    }
    if (m_cfa == LLDB_INVALID_ADDRESS)
    {
        return false;
    }
    cfa = m_cfa;
    return true;
}

// Retrieve the address of the start of the function of THIS frame

bool
RegisterContextLLDB::GetStartPC (addr_t& start_pc)
{
    if (!IsValid())
        return false;
    if (!m_start_pc.IsValid())
    {
        return GetPC (start_pc); 
    }
    start_pc = m_start_pc.GetLoadAddress (&m_thread.GetProcess().GetTarget());
    return true;
}

// Retrieve the current pc value for THIS frame, as saved by the NEXT frame.

bool
RegisterContextLLDB::GetPC (addr_t& pc)
{
    if (!IsValid())
        return false;
    if (ReadGPRValue (eRegisterKindGeneric, LLDB_REGNUM_GENERIC_PC, pc))
    {
        // A pc value of 0 or 1 is impossible in the middle of the stack -- it indicates the end of a stack walk.
        // On the currently executing frame (or such a frame interrupted asynchronously by sigtramp et al) this may
        // occur if code has jumped through a NULL pointer -- we want to be able to unwind past that frame to help
        // find the bug.

        if (m_all_registers_available == false 
            && (pc == 0 || pc == 1))
        {
            return false;
        }
        else 
        {
            return true;
        }
    }
    else
    {
        return false;
    }
}
