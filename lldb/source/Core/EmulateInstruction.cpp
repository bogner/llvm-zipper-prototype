//===-- EmulateInstruction.h ------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "lldb/Core/EmulateInstruction.h"

#include "lldb/Core/Address.h"
#include "lldb/Core/DataBufferHeap.h"
#include "lldb/Core/DataExtractor.h"
#include "lldb/Core/Error.h"
#include "lldb/Core/PluginManager.h"
#include "lldb/Core/StreamString.h"
#include "lldb/Host/Endian.h"
#include "lldb/Symbol/UnwindPlan.h"
#include "lldb/Target/Process.h"
#include "lldb/Target/RegisterContext.h"
#include "lldb/Target/Target.h"
#include "lldb/Target/Thread.h"

using namespace lldb;
using namespace lldb_private;

EmulateInstruction*
EmulateInstruction::FindPlugin (const ArchSpec &arch, InstructionType supported_inst_type, const char *plugin_name)
{
    EmulateInstructionCreateInstance create_callback = NULL;
    if (plugin_name)
    {
        create_callback  = PluginManager::GetEmulateInstructionCreateCallbackForPluginName (plugin_name);
        if (create_callback)
        {
           	EmulateInstruction *emulate_insn_ptr = create_callback(arch, supported_inst_type);
            if (emulate_insn_ptr)
                return emulate_insn_ptr;
        }
    }
    else
    {
        for (uint32_t idx = 0; (create_callback = PluginManager::GetEmulateInstructionCreateCallbackAtIndex(idx)) != NULL; ++idx)
        {
            EmulateInstruction *emulate_insn_ptr = create_callback(arch, supported_inst_type);
            if (emulate_insn_ptr)
                return emulate_insn_ptr;
        }
    }
    return NULL;
}

EmulateInstruction::EmulateInstruction (const ArchSpec &arch) :
    m_arch (arch),
    m_baton (NULL),
    m_read_mem_callback (&ReadMemoryDefault),
    m_write_mem_callback (&WriteMemoryDefault),
    m_read_reg_callback (&ReadRegisterDefault),
    m_write_reg_callback (&WriteRegisterDefault),
    m_opcode_pc (LLDB_INVALID_ADDRESS)
{
    ::memset (&m_opcode, 0, sizeof (m_opcode));
}


uint64_t
EmulateInstruction::ReadRegisterUnsigned (uint32_t reg_kind, uint32_t reg_num, uint64_t fail_value, bool *success_ptr)
{
    RegisterInfo reg_info;
    if (GetRegisterInfo(reg_kind, reg_num, reg_info))
        return ReadRegisterUnsigned (reg_info, fail_value, success_ptr);
    if (success_ptr)
        *success_ptr = false;
    return fail_value;
}

uint64_t
EmulateInstruction::ReadRegisterUnsigned (const RegisterInfo &reg_info, uint64_t fail_value, bool *success_ptr)
{
    uint64_t uval64 = 0;
    bool success = m_read_reg_callback (this, m_baton, reg_info, uval64);
    if (success_ptr)
        *success_ptr = success;
    if (!success)
        uval64 = fail_value;
    return uval64;
}

bool
EmulateInstruction::WriteRegisterUnsigned (const Context &context, uint32_t reg_kind, uint32_t reg_num, uint64_t reg_value)
{
    RegisterInfo reg_info;
    if (GetRegisterInfo(reg_kind, reg_num, reg_info))
        return WriteRegisterUnsigned (context, reg_info, reg_value);
    return false;
}

bool
EmulateInstruction::WriteRegisterUnsigned (const Context &context, const RegisterInfo &reg_info, uint64_t reg_value)
{
    return m_write_reg_callback (this, m_baton, context, reg_info, reg_value);    
}

uint64_t
EmulateInstruction::ReadMemoryUnsigned (const Context &context, lldb::addr_t addr, size_t byte_size, uint64_t fail_value, bool *success_ptr)
{
    uint64_t uval64 = 0;
    bool success = false;
    if (byte_size <= 8)
    {
        uint8_t buf[sizeof(uint64_t)];
        size_t bytes_read = m_read_mem_callback (this, m_baton, context, addr, buf, byte_size);
        if (bytes_read == byte_size)
        {
            uint32_t offset = 0;
            DataExtractor data (buf, byte_size, GetByteOrder(), GetAddressByteSize());
            uval64 = data.GetMaxU64 (&offset, byte_size);
            success = true;
        }
    }

    if (success_ptr)
        *success_ptr = success;

    if (!success)
        uval64 = fail_value;
    return uval64;
}


bool
EmulateInstruction::WriteMemoryUnsigned (const Context &context, 
                                         lldb::addr_t addr, 
                                         uint64_t uval,
                                         size_t uval_byte_size)
{
    StreamString strm(Stream::eBinary, GetAddressByteSize(), GetByteOrder());
    strm.PutMaxHex64 (uval, uval_byte_size);
    
    size_t bytes_written = m_write_mem_callback (this, m_baton, context, addr, strm.GetData(), uval_byte_size);
    if (bytes_written == uval_byte_size)
        return true;
    return false;
}


void
EmulateInstruction::SetBaton (void *baton)
{
    m_baton = baton;
}

void
EmulateInstruction::SetCallbacks (ReadMemory read_mem_callback,
                                  WriteMemory write_mem_callback,
                                  ReadRegister read_reg_callback,
                                  WriteRegister write_reg_callback)
{
    m_read_mem_callback = read_mem_callback;
    m_write_mem_callback = write_mem_callback;
    m_read_reg_callback = read_reg_callback;
    m_write_reg_callback = write_reg_callback;
}

void
EmulateInstruction::SetReadMemCallback (ReadMemory read_mem_callback)
{
    m_read_mem_callback = read_mem_callback;
}

                                  
void
EmulateInstruction::SetWriteMemCallback (WriteMemory write_mem_callback)
{
    m_write_mem_callback = write_mem_callback;
}

                                  
void
EmulateInstruction::SetReadRegCallback (ReadRegister read_reg_callback)
{
    m_read_reg_callback = read_reg_callback;
}

                                  
void
EmulateInstruction::SetWriteRegCallback (WriteRegister write_reg_callback)
{
    m_write_reg_callback = write_reg_callback;
}

                                  
                            
//
//  Read & Write Memory and Registers callback functions.
//

size_t 
EmulateInstruction::ReadMemoryFrame (EmulateInstruction *instruction,
                                     void *baton,
                                     const Context &context, 
                                     lldb::addr_t addr, 
                                     void *dst,
                                     size_t length)
{
    if (!baton)
        return 0;
    
    
    StackFrame *frame = (StackFrame *) baton;

    DataBufferSP data_sp (new DataBufferHeap (length, '\0'));
    Error error;
    
    size_t bytes_read = frame->GetThread().GetProcess().ReadMemory (addr, data_sp->GetBytes(), data_sp->GetByteSize(),
                                                                    error);
    
    if (bytes_read > 0)
        ((DataBufferHeap *) data_sp.get())->CopyData (dst, length);
        
    return bytes_read;
}

size_t 
EmulateInstruction::WriteMemoryFrame (EmulateInstruction *instruction,
                                      void *baton,
                                      const Context &context, 
                                      lldb::addr_t addr, 
                                      const void *dst,
                                      size_t length)
{
    if (!baton)
        return 0;
    
    StackFrame *frame = (StackFrame *) baton;

    lldb::DataBufferSP data_sp (new DataBufferHeap (dst, length));
    if (data_sp)
    {
        length = data_sp->GetByteSize();
        if (length > 0)
        {
            Error error;
            size_t bytes_written = frame->GetThread().GetProcess().WriteMemory (addr, data_sp->GetBytes(), length, 
                                                                                error);
            
            return bytes_written;
        }
    }
    
    return 0;
}

bool   
EmulateInstruction::ReadRegisterFrame  (EmulateInstruction *instruction,
                                        void *baton,
                                        const RegisterInfo &reg_info,
                                        uint64_t &reg_value)
{
    if (!baton)
        return false;
        
    StackFrame *frame = (StackFrame *) baton;
    RegisterContext *reg_ctx = frame->GetRegisterContext().get();
    Scalar value;
    
    const uint32_t internal_reg_num = GetInternalRegisterNumber (reg_ctx, reg_info);
    
    if (internal_reg_num != LLDB_INVALID_REGNUM)
    {
        if (reg_ctx->ReadRegisterValue (internal_reg_num, value))
        {
            reg_value = value.GetRawBits64 (0);
            return true;
        }
    }
    return false;
}

bool   
EmulateInstruction::WriteRegisterFrame (EmulateInstruction *instruction,
                                        void *baton,
                                        const Context &context, 
                                        const RegisterInfo &reg_info,
                                        uint64_t reg_value)
{
    if (!baton)
        return false;
        
    StackFrame *frame = (StackFrame *) baton;
    RegisterContext *reg_ctx = frame->GetRegisterContext().get();
    Scalar value (reg_value);
    const uint32_t internal_reg_num = GetInternalRegisterNumber (reg_ctx, reg_info);    
    if (internal_reg_num != LLDB_INVALID_REGNUM)
        return reg_ctx->WriteRegisterValue (internal_reg_num, value);
    return false;
}

size_t 
EmulateInstruction::ReadMemoryDefault (EmulateInstruction *instruction,
                                       void *baton,
                                       const Context &context, 
                                       lldb::addr_t addr, 
                                       void *dst,
                                       size_t length)
{
    fprintf (stdout, "    Read from Memory (address = 0x%llx, length = %zu, context = ", addr, (uint32_t) length);
    context.Dump (stdout, instruction);    
    *((uint64_t *) dst) = 0xdeadbeef;
    return length;
}

size_t 
EmulateInstruction::WriteMemoryDefault (EmulateInstruction *instruction,
                                        void *baton,
                                        const Context &context, 
                                        lldb::addr_t addr, 
                                        const void *dst,
                                        size_t length)
{
    fprintf (stdout, "    Write to Memory (address = 0x%llx, length = %zu, context = ", addr, (uint32_t) length);
    context.Dump (stdout, instruction);    
    return length;
}

bool   
EmulateInstruction::ReadRegisterDefault  (EmulateInstruction *instruction,
                                          void *baton,
                                          const RegisterInfo &reg_info,
                                          uint64_t &reg_value)
{
    fprintf (stdout, "  Read Register (%s)\n", reg_info.name);
    uint32_t reg_kind, reg_num;
    if (GetBestRegisterKindAndNumber (reg_info, reg_kind, reg_num))
        reg_value = (uint64_t)reg_kind << 24 | reg_num;
    else
        reg_value = 0;

    return true;
}

bool   
EmulateInstruction::WriteRegisterDefault (EmulateInstruction *instruction,
                                          void *baton,
                                          const Context &context, 
                                          const RegisterInfo &reg_info,
                                          uint64_t reg_value)
{
    fprintf (stdout, "    Write to Register (name = %s, value = 0x%llx, context = ", reg_info.name, reg_value);
    context.Dump (stdout, instruction);        
    return true;
}

void
EmulateInstruction::Context::Dump (FILE *fh, 
                                   EmulateInstruction *instruction) const
{
    switch (type)
    {
        case eContextReadOpcode:
            fprintf (fh, "reading opcode");
            break;
            
        case eContextImmediate:
            fprintf (fh, "immediate");
            break;
            
        case eContextPushRegisterOnStack:
            fprintf (fh, "push register");
            break;
            
        case eContextPopRegisterOffStack:
            fprintf (fh, "pop register");
            break;
            
        case eContextAdjustStackPointer:
            fprintf (fh, "adjust sp");
            break;
            
        case eContextAdjustBaseRegister:
            fprintf (fh, "adjusting (writing value back to) a base register");
            break;
            
        case eContextRegisterPlusOffset:
            fprintf (fh, "register + offset");
            break;
            
        case eContextRegisterStore:
            fprintf (fh, "store register");
            break;
            
        case eContextRegisterLoad:
            fprintf (fh, "load register");
            break;
            
        case eContextRelativeBranchImmediate:
            fprintf (fh, "relative branch immediate");
            break;
            
        case eContextAbsoluteBranchRegister:
            fprintf (fh, "absolute branch register");
            break;
            
        case eContextSupervisorCall:
            fprintf (fh, "supervisor call");
            break;
            
        case eContextTableBranchReadMemory:
            fprintf (fh, "table branch read memory");
            break;
            
        case eContextWriteRegisterRandomBits:
            fprintf (fh, "write random bits to a register");
            break;
            
        case eContextWriteMemoryRandomBits:
            fprintf (fh, "write random bits to a memory address");
            break;
            
        case eContextArithmetic:
            fprintf (fh, "arithmetic");
            break;
            
        case eContextReturnFromException:
            fprintf (fh, "return from exception");
            break;
            
        default:
            fprintf (fh, "unrecognized context.");
            break;
    }
    
    switch (info_type)
    {
    case eInfoTypeRegisterPlusOffset:
        {
            fprintf (fh, 
                     " (reg_plus_offset = %s%+lld)\n",
                     info.RegisterPlusOffset.reg.name,
                     info.RegisterPlusOffset.signed_offset);
        }
        break;

    case eInfoTypeRegisterPlusIndirectOffset:
        {
            fprintf (fh, " (reg_plus_reg = %s + %s)\n",
                     info.RegisterPlusIndirectOffset.base_reg.name,
                     info.RegisterPlusIndirectOffset.offset_reg.name);
        }
        break;

    case eInfoTypeRegisterToRegisterPlusOffset:
        {
            fprintf (fh, " (base_and_imm_offset = %s%+lld, data_reg = %s)\n", 
                     info.RegisterToRegisterPlusOffset.base_reg.name, 
                     info.RegisterToRegisterPlusOffset.offset,
                     info.RegisterToRegisterPlusOffset.data_reg.name);
        }
        break;

    case eInfoTypeRegisterToRegisterPlusIndirectOffset:
        {
            fprintf (fh, " (base_and_reg_offset = %s + %s, data_reg = %s)\n",
                     info.RegisterToRegisterPlusIndirectOffset.base_reg.name, 
                     info.RegisterToRegisterPlusIndirectOffset.offset_reg.name, 
                     info.RegisterToRegisterPlusIndirectOffset.data_reg.name);
        }
        break;
    
    case eInfoTypeRegisterRegisterOperands:
        {
            fprintf (fh, " (register to register binary op: %s and %s)\n", 
                     info.RegisterRegisterOperands.operand1.name,
                     info.RegisterRegisterOperands.operand2.name);
        }
        break;

    case eInfoTypeOffset:
        fprintf (fh, " (signed_offset = %+lld)\n", info.signed_offset);
        break;
        
    case eInfoTypeRegister:
        fprintf (fh, " (reg = %s)\n", info.reg.name);
        break;
        
    case eInfoTypeImmediate:
        fprintf (fh, 
                 " (unsigned_immediate = %llu (0x%16.16llx))\n", 
                 info.unsigned_immediate, 
                 info.unsigned_immediate);
        break;

    case eInfoTypeImmediateSigned:
        fprintf (fh, 
                 " (signed_immediate = %+lld (0x%16.16llx))\n", 
                 info.signed_immediate, 
                 info.signed_immediate);
        break;
        
    case eInfoTypeAddress:
        fprintf (fh, " (address = 0x%llx)\n", info.address);
        break;
        
    case eInfoTypeISAAndImmediate:
        fprintf (fh, 
                 " (isa = %u, unsigned_immediate = %u (0x%8.8x))\n", 
                 info.ISAAndImmediate.isa,
                 info.ISAAndImmediate.unsigned_data32,
                 info.ISAAndImmediate.unsigned_data32);
        break;
        
    case eInfoTypeISAAndImmediateSigned:
        fprintf (fh,
                 " (isa = %u, signed_immediate = %i (0x%8.8x))\n", 
                 info.ISAAndImmediateSigned.isa,
                 info.ISAAndImmediateSigned.signed_data32,
                 info.ISAAndImmediateSigned.signed_data32);
        break;
        
    case eInfoTypeISA:
        fprintf (fh, " (isa = %u)\n", info.isa);
        break;
        
    case eInfoTypeNoArgs:
        fprintf (fh, " \n");
        break;

    default:
        fprintf (fh, " (unknown <info_type>)\n");
        break;
    }
}

bool
EmulateInstruction::SetInstruction (const Opcode &opcode, const Address &inst_addr, Target *target)
{
    m_opcode = opcode;
    m_opcode_pc = LLDB_INVALID_ADDRESS;
    if (inst_addr.IsValid())
    {
        if (target)
            m_opcode_pc = inst_addr.GetLoadAddress (target);
        if (m_opcode_pc == LLDB_INVALID_ADDRESS)
            m_opcode_pc = inst_addr.GetFileAddress ();
    }
    return true;
}

bool
EmulateInstruction::GetBestRegisterKindAndNumber (const RegisterInfo &reg_info, 
                                                  uint32_t &reg_kind,
                                                  uint32_t &reg_num)
{
    // Generic and DWARF should be the two most popular register kinds when
    // emulating instructions since they are the most platform agnostic...
    reg_num = reg_info.kinds[eRegisterKindGeneric];
    if (reg_num != LLDB_INVALID_REGNUM)
    {
        reg_kind = eRegisterKindGeneric;
        return true;
    }
    
    reg_num = reg_info.kinds[eRegisterKindDWARF];
    if (reg_num != LLDB_INVALID_REGNUM)
    {
        reg_kind = eRegisterKindDWARF;
        return true;
    }

    reg_num = reg_info.kinds[eRegisterKindLLDB];
    if (reg_num != LLDB_INVALID_REGNUM)
    {
        reg_kind = eRegisterKindLLDB;
        return true;
    }

    reg_num = reg_info.kinds[eRegisterKindGCC];
    if (reg_num != LLDB_INVALID_REGNUM)
    {
        reg_kind = eRegisterKindGCC;
        return true;
    }

    reg_num = reg_info.kinds[eRegisterKindGDB];
    if (reg_num != LLDB_INVALID_REGNUM)
    {
        reg_kind = eRegisterKindGDB;
        return true;
    }
    return false;
}

uint32_t
EmulateInstruction::GetInternalRegisterNumber (RegisterContext *reg_ctx, const RegisterInfo &reg_info)
{
    uint32_t reg_kind, reg_num;
    if (reg_ctx && GetBestRegisterKindAndNumber (reg_info, reg_kind, reg_num))
        return reg_ctx->ConvertRegisterKindToRegisterNumber (reg_kind, reg_num);
    return LLDB_INVALID_REGNUM;
}


bool
EmulateInstruction::CreateFunctionEntryUnwind (UnwindPlan &unwind_plan)
{
    unwind_plan.Clear();
    return false;
}


