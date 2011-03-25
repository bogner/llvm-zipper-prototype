//===-- Disassembler.cpp ----------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "lldb/Core/Disassembler.h"

// C Includes
// C++ Includes
// Other libraries and framework includes
// Project includes
#include "lldb/lldb-private.h"
#include "lldb/Core/Error.h"
#include "lldb/Core/DataBufferHeap.h"
#include "lldb/Core/DataExtractor.h"
#include "lldb/Core/Debugger.h"
#include "lldb/Core/Module.h"
#include "lldb/Core/PluginManager.h"
#include "lldb/Core/Timer.h"
#include "lldb/Symbol/ObjectFile.h"
#include "lldb/Target/ExecutionContext.h"
#include "lldb/Target/Process.h"
#include "lldb/Target/StackFrame.h"
#include "lldb/Target/Target.h"

#define DEFAULT_DISASM_BYTE_SIZE 32

using namespace lldb;
using namespace lldb_private;


Disassembler*
Disassembler::FindPlugin (const ArchSpec &arch, const char *plugin_name)
{
    Timer scoped_timer (__PRETTY_FUNCTION__,
                        "Disassembler::FindPlugin (arch = %s, plugin_name = %s)",
                        arch.GetArchitectureName(),
                        plugin_name);

    std::auto_ptr<Disassembler> disassembler_ap;
    DisassemblerCreateInstance create_callback = NULL;
    
    if (plugin_name)
    {
        create_callback = PluginManager::GetDisassemblerCreateCallbackForPluginName (plugin_name);
        if (create_callback)
        {
            disassembler_ap.reset (create_callback(arch));
            
            if (disassembler_ap.get())
                return disassembler_ap.release();
        }
    }
    else
    {
        for (uint32_t idx = 0; (create_callback = PluginManager::GetDisassemblerCreateCallbackAtIndex(idx)) != NULL; ++idx)
        {
            disassembler_ap.reset (create_callback(arch));

            if (disassembler_ap.get())
                return disassembler_ap.release();
        }
    }
    return NULL;
}



size_t
Disassembler::Disassemble
(
    Debugger &debugger,
    const ArchSpec &arch,
    const char *plugin_name,
    const ExecutionContext &exe_ctx,
    SymbolContextList &sc_list,
    uint32_t num_instructions,
    uint32_t num_mixed_context_lines,
    bool show_bytes,
    bool raw,
    Stream &strm
)
{
    size_t success_count = 0;
    const size_t count = sc_list.GetSize();
    SymbolContext sc;
    AddressRange range;
    
    for (size_t i=0; i<count; ++i)
    {
        if (sc_list.GetContextAtIndex(i, sc) == false)
            break;
        if (sc.GetAddressRange(eSymbolContextFunction | eSymbolContextSymbol, range))
        {
            if (Disassemble (debugger, 
                             arch, 
                             plugin_name,
                             exe_ctx, 
                             range, 
                             num_instructions,
                             num_mixed_context_lines, 
                             show_bytes, 
                             raw, 
                             strm))
            {
                ++success_count;
                strm.EOL();
            }
        }
    }
    return success_count;
}

bool
Disassembler::Disassemble
(
    Debugger &debugger,
    const ArchSpec &arch,
    const char *plugin_name,
    const ExecutionContext &exe_ctx,
    const ConstString &name,
    Module *module,
    uint32_t num_instructions,
    uint32_t num_mixed_context_lines,
    bool show_bytes,
    bool raw,
    Stream &strm
)
{
    SymbolContextList sc_list;
    if (name)
    {
        const bool include_symbols = true;
        if (module)
        {
            module->FindFunctions (name, 
                                   eFunctionNameTypeBase | 
                                   eFunctionNameTypeFull | 
                                   eFunctionNameTypeMethod | 
                                   eFunctionNameTypeSelector, 
                                   include_symbols,
                                   true,
                                   sc_list);
        }
        else if (exe_ctx.target)
        {
            exe_ctx.target->GetImages().FindFunctions (name, 
                                                       eFunctionNameTypeBase | 
                                                       eFunctionNameTypeFull | 
                                                       eFunctionNameTypeMethod | 
                                                       eFunctionNameTypeSelector,
                                                       include_symbols, 
                                                       false,
                                                       sc_list);
        }
    }
    
    if (sc_list.GetSize ())
    {
        return Disassemble (debugger, 
                            arch, 
                            plugin_name,
                            exe_ctx, 
                            sc_list,
                            num_instructions, 
                            num_mixed_context_lines, 
                            show_bytes,
                            raw,
                            strm);
    }
    return false;
}


lldb::DisassemblerSP
Disassembler::DisassembleRange
(
    const ArchSpec &arch,
    const char *plugin_name,
    const ExecutionContext &exe_ctx,
    const AddressRange &range
)
{
    lldb::DisassemblerSP disasm_sp;
    if (range.GetByteSize() > 0 && range.GetBaseAddress().IsValid())
    {
        disasm_sp.reset (Disassembler::FindPlugin(arch, plugin_name));

        if (disasm_sp)
        {
            DataExtractor data;
            size_t bytes_disassembled = disasm_sp->ParseInstructions (&exe_ctx, range, data);
            if (bytes_disassembled == 0)
                disasm_sp.reset();
        }
    }
    return disasm_sp;
}


bool
Disassembler::Disassemble
(
    Debugger &debugger,
    const ArchSpec &arch,
    const char *plugin_name,
    const ExecutionContext &exe_ctx,
    const AddressRange &disasm_range,
    uint32_t num_instructions,
    uint32_t num_mixed_context_lines,
    bool show_bytes,
    bool raw,
    Stream &strm
)
{
    if (disasm_range.GetByteSize())
    {
        std::auto_ptr<Disassembler> disasm_ap (Disassembler::FindPlugin(arch, plugin_name));

        if (disasm_ap.get())
        {
            AddressRange range(disasm_range);
            
            // If we weren't passed in a section offset address range,
            // try and resolve it to something
            if (range.GetBaseAddress().IsSectionOffset() == false)
            {
                if (exe_ctx.target)
                {
                    if (exe_ctx.target->GetSectionLoadList().IsEmpty())
                    {
                        exe_ctx.target->GetImages().ResolveFileAddress (range.GetBaseAddress().GetOffset(), range.GetBaseAddress());
                    }
                    else
                    {
                        exe_ctx.target->GetSectionLoadList().ResolveLoadAddress (range.GetBaseAddress().GetOffset(), range.GetBaseAddress());
                    }
                }
            }

            DataExtractor data;
            size_t bytes_disassembled = disasm_ap->ParseInstructions (&exe_ctx, range, data);
            if (bytes_disassembled == 0)
                return false;

            return PrintInstructions (disasm_ap.get(),
                                      debugger,
                                      arch,
                                      exe_ctx,
                                      disasm_range.GetBaseAddress(),
                                      num_instructions,
                                      num_mixed_context_lines,
                                      show_bytes,
                                      raw,
                                      strm);
        }
    }
    return false;
}
            
bool
Disassembler::Disassemble
(
    Debugger &debugger,
    const ArchSpec &arch,
    const char *plugin_name,
    const ExecutionContext &exe_ctx,
    const Address &start_address,
    uint32_t num_instructions,
    uint32_t num_mixed_context_lines,
    bool show_bytes,
    bool raw,
    Stream &strm
)
{
    if (num_instructions > 0)
    {
        std::auto_ptr<Disassembler> disasm_ap (Disassembler::FindPlugin(arch, plugin_name));
        Address addr = start_address;

        if (disasm_ap.get())
        {
            // If we weren't passed in a section offset address range,
            // try and resolve it to something
            if (addr.IsSectionOffset() == false)
            {
                if (exe_ctx.target)
                {
                    if (exe_ctx.target->GetSectionLoadList().IsEmpty())
                    {
                        exe_ctx.target->GetImages().ResolveFileAddress (addr.GetOffset(), addr);
                    }
                    else
                    {
                        exe_ctx.target->GetSectionLoadList().ResolveLoadAddress (addr.GetOffset(), addr);
                    }
                }
            }

            DataExtractor data;
            size_t bytes_disassembled = disasm_ap->ParseInstructions (&exe_ctx, addr, num_instructions, data);
            if (bytes_disassembled == 0)
                return false;
            return PrintInstructions (disasm_ap.get(),
                                      debugger,
                                      arch,
                                      exe_ctx,
                                      addr,
                                      num_instructions,
                                      num_mixed_context_lines,
                                      show_bytes,
                                      raw,
                                      strm);
        }
    }
    return false;
}
            
bool 
Disassembler::PrintInstructions
(
    Disassembler *disasm_ptr,
    Debugger &debugger,
    const ArchSpec &arch,
    const ExecutionContext &exe_ctx,
    const Address &start_addr,
    uint32_t num_instructions,
    uint32_t num_mixed_context_lines,
    bool show_bytes,
    bool raw,
    Stream &strm
)
{
    // We got some things disassembled...
    size_t num_instructions_found = disasm_ptr->GetInstructionList().GetSize();
    
    if (num_instructions > 0 && num_instructions < num_instructions_found)
        num_instructions_found = num_instructions;
        
    uint32_t offset = 0;
    SymbolContext sc;
    SymbolContext prev_sc;
    AddressRange sc_range;
    Address addr = start_addr;
    
    if (num_mixed_context_lines)
        strm.IndentMore ();

    // We extract the section to make sure we don't transition out
    // of the current section when disassembling
    const Section *addr_section = addr.GetSection();
    Module *range_module = addr.GetModule();

    for (size_t i=0; i<num_instructions_found; ++i)
    {
        Instruction *inst = disasm_ptr->GetInstructionList().GetInstructionAtIndex (i).get();
        if (inst)
        {
            addr_t file_addr = addr.GetFileAddress();
            if (addr_section == NULL || addr_section->ContainsFileAddress (file_addr) == false)
            {
                if (range_module)
                    range_module->ResolveFileAddress (file_addr, addr);
                else if (exe_ctx.target)
                    exe_ctx.target->GetImages().ResolveFileAddress (file_addr, addr);
                    
                addr_section = addr.GetSection();
            }

            prev_sc = sc;

            if (addr_section)
            {
                Module *module = addr_section->GetModule();
                uint32_t resolved_mask = module->ResolveSymbolContextForAddress(addr, eSymbolContextEverything, sc);
                if (resolved_mask)
                {
                    if (!(prev_sc.function == sc.function || prev_sc.symbol == sc.symbol))
                    {
                        if (prev_sc.function || prev_sc.symbol)
                            strm.EOL();

                        strm << sc.module_sp->GetFileSpec().GetFilename();
                        
                        if (sc.function)
                            strm << '`' << sc.function->GetMangled().GetName();
                        else if (sc.symbol)
                            strm << '`' << sc.symbol->GetMangled().GetName();
                        strm << ":\n";
                    }

                    if (num_mixed_context_lines && !sc_range.ContainsFileAddress (addr))
                    {
                        sc.GetAddressRange (eSymbolContextEverything, sc_range);
                            
                        if (sc != prev_sc)
                        {
                            if (offset != 0)
                                strm.EOL();

                            sc.DumpStopContext(&strm, exe_ctx.process, addr, false, true, false);
                            strm.EOL();

                            if (sc.comp_unit && sc.line_entry.IsValid())
                            {
                                debugger.GetSourceManager().DisplaySourceLinesWithLineNumbers (sc.line_entry.file,
                                                                                               sc.line_entry.line,
                                                                                               num_mixed_context_lines,
                                                                                               num_mixed_context_lines,
                                                                                               num_mixed_context_lines ? "->" : "",
                                                                                               &strm);
                            }
                        }
                    }
                }
                else
                {
                    sc.Clear();
                }
            }
            if (num_mixed_context_lines)
                strm.IndentMore ();
            strm.Indent();
            inst->Dump(&strm, true, show_bytes, &exe_ctx, raw);
            strm.EOL();
            
            addr.Slide(inst->GetOpcode().GetByteSize());

            if (num_mixed_context_lines)
                strm.IndentLess ();
        }
        else
        {
            break;
        }
    }
    if (num_mixed_context_lines)
        strm.IndentLess ();
        
    return true;
}


bool
Disassembler::Disassemble
(
    Debugger &debugger,
    const ArchSpec &arch,
    const char *plugin_name,
    const ExecutionContext &exe_ctx,
    uint32_t num_instructions,
    uint32_t num_mixed_context_lines,
    bool show_bytes,
    bool raw,
    Stream &strm
)
{
    AddressRange range;
    if (exe_ctx.frame)
    {
        SymbolContext sc(exe_ctx.frame->GetSymbolContext(eSymbolContextFunction | eSymbolContextSymbol));
        if (sc.function)
        {
            range = sc.function->GetAddressRange();
        }
        else if (sc.symbol && sc.symbol->GetAddressRangePtr())
        {
            range = *sc.symbol->GetAddressRangePtr();
        }
        else
        {
            range.GetBaseAddress() = exe_ctx.frame->GetFrameCodeAddress();
        }

        if (range.GetBaseAddress().IsValid() && range.GetByteSize() == 0)
            range.SetByteSize (DEFAULT_DISASM_BYTE_SIZE);
    }

    return Disassemble (debugger, 
                        arch, 
                        plugin_name,
                        exe_ctx, 
                        range, 
                        num_instructions, 
                        num_mixed_context_lines, 
                        show_bytes, 
                        raw, 
                        strm);
}

Instruction::Instruction(const Address &address) :
    m_address (address),
    m_opcode()
{
}

Instruction::~Instruction()
{
}


InstructionList::InstructionList() :
    m_instructions()
{
}

InstructionList::~InstructionList()
{
}

size_t
InstructionList::GetSize() const
{
    return m_instructions.size();
}


InstructionSP
InstructionList::GetInstructionAtIndex (uint32_t idx) const
{
    InstructionSP inst_sp;
    if (idx < m_instructions.size())
        inst_sp = m_instructions[idx];
    return inst_sp;
}

void
InstructionList::Clear()
{
  m_instructions.clear();
}

void
InstructionList::Append (lldb::InstructionSP &inst_sp)
{
    if (inst_sp)
        m_instructions.push_back(inst_sp);
}


size_t
Disassembler::ParseInstructions
(
    const ExecutionContext *exe_ctx,
    const AddressRange &range,
    DataExtractor& data
)
{
    Target *target = exe_ctx->target;
    const addr_t byte_size = range.GetByteSize();
    if (target == NULL || byte_size == 0 || !range.GetBaseAddress().IsValid())
        return 0;

    DataBufferHeap *heap_buffer = new DataBufferHeap (byte_size, '\0');
    DataBufferSP data_sp(heap_buffer);

    Error error;
    bool prefer_file_cache = true;
    const size_t bytes_read = target->ReadMemory (range.GetBaseAddress(), prefer_file_cache, heap_buffer->GetBytes(), heap_buffer->GetByteSize(), error);
    
    if (bytes_read > 0)
    {
        if (bytes_read != heap_buffer->GetByteSize())
            heap_buffer->SetByteSize (bytes_read);

        data.SetData(data_sp);
        data.SetByteOrder(target->GetArchitecture().GetByteOrder());
        data.SetAddressByteSize(target->GetArchitecture().GetAddressByteSize());
        return DecodeInstructions (range.GetBaseAddress(), data, 0, UINT32_MAX, false);
    }

    return 0;
}

size_t
Disassembler::ParseInstructions
(
    const ExecutionContext *exe_ctx,
    const Address &start,
    uint32_t num_instructions,
    DataExtractor& data
)
{
    Address addr = start;
    
    if (num_instructions == 0)
        return 0;
        
    Target *target = exe_ctx->target;
    // We'll guess at a size for the buffer, if we don't get all the instructions we want we can just re-fill & reuse it.
    const addr_t byte_size = num_instructions * 2;
    addr_t data_offset = 0;
    addr_t next_instruction_offset = 0;
    size_t buffer_size = byte_size;
    
    uint32_t num_instructions_found = 0;
    
    if (target == NULL || byte_size == 0 || !start.IsValid())
        return 0;

    DataBufferHeap *heap_buffer = new DataBufferHeap (byte_size, '\0');
    DataBufferSP data_sp(heap_buffer);
    
    data.SetData(data_sp);
    data.SetByteOrder(target->GetArchitecture().GetByteOrder());
    data.SetAddressByteSize(target->GetArchitecture().GetAddressByteSize());

    Error error;
    bool prefer_file_cache = true;
    
    m_instruction_list.Clear();
    
    while (num_instructions_found < num_instructions)
    {
        if (buffer_size < data_offset + byte_size)
        {
            buffer_size = data_offset + byte_size;
            heap_buffer->SetByteSize (buffer_size);
            data.SetData(data_sp);  // Resizing might have changed the backing store location, so we have to reset
                                    // the DataBufferSP in the extractor so it changes to pointing at the right thing.
        }
        const size_t bytes_read = target->ReadMemory (addr, prefer_file_cache, heap_buffer->GetBytes() + data_offset, byte_size, error);
        size_t num_bytes_read = 0;
        if (bytes_read == 0)
            break;
            
        num_bytes_read = DecodeInstructions (start, data, next_instruction_offset, num_instructions - num_instructions_found, true);
        if (num_bytes_read == 0)
            break;
        num_instructions_found = m_instruction_list.GetSize();
        
        // Prepare for the next round.
        data_offset += bytes_read;
        addr.Slide (bytes_read);
        next_instruction_offset += num_bytes_read;
    }
    
    return m_instruction_list.GetSize();
}

//----------------------------------------------------------------------
// Disassembler copy constructor
//----------------------------------------------------------------------
Disassembler::Disassembler(const ArchSpec& arch) :
    m_arch (arch),
    m_instruction_list(),
    m_base_addr(LLDB_INVALID_ADDRESS)
{

}

//----------------------------------------------------------------------
// Destructor
//----------------------------------------------------------------------
Disassembler::~Disassembler()
{
}

InstructionList &
Disassembler::GetInstructionList ()
{
    return m_instruction_list;
}

const InstructionList &
Disassembler::GetInstructionList () const
{
    return m_instruction_list;
}
