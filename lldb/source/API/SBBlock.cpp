//===-- SBBlock.cpp ---------------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "lldb/API/SBBlock.h"
#include "lldb/API/SBAddress.h"
#include "lldb/API/SBFileSpec.h"
#include "lldb/API/SBStream.h"
#include "lldb/Core/AddressRange.h"
#include "lldb/Symbol/Block.h"
#include "lldb/Symbol/Function.h"
#include "lldb/Symbol/SymbolContext.h"

using namespace lldb;
using namespace lldb_private;


SBBlock::SBBlock () :
    m_opaque_ptr (NULL)
{
}

SBBlock::SBBlock (lldb_private::Block *lldb_object_ptr) :
    m_opaque_ptr (lldb_object_ptr)
{
}

SBBlock::SBBlock(const SBBlock &rhs) :
    m_opaque_ptr (rhs.m_opaque_ptr)
{
}

const SBBlock &
SBBlock::operator = (const SBBlock &rhs)
{
    m_opaque_ptr = rhs.m_opaque_ptr;
    return *this;
}

SBBlock::~SBBlock ()
{
    m_opaque_ptr = NULL;
}

bool
SBBlock::IsValid () const
{
    return m_opaque_ptr != NULL;
}

bool
SBBlock::IsInlined () const
{
    if (m_opaque_ptr)
        return m_opaque_ptr->GetInlinedFunctionInfo () != NULL;
    return false;
}

const char *
SBBlock::GetInlinedName () const
{
    if (m_opaque_ptr)
    {
        const InlineFunctionInfo* inlined_info = m_opaque_ptr->GetInlinedFunctionInfo ();
        if (inlined_info)
            return inlined_info->GetName().AsCString (NULL);
    }
    return NULL;
}

SBFileSpec
SBBlock::GetInlinedCallSiteFile () const
{
    SBFileSpec sb_file;
    if (m_opaque_ptr)
    {
        const InlineFunctionInfo* inlined_info = m_opaque_ptr->GetInlinedFunctionInfo ();
        if (inlined_info)
            sb_file.SetFileSpec (inlined_info->GetCallSite().GetFile());
    }
    return sb_file;
}

uint32_t
SBBlock::GetInlinedCallSiteLine () const
{
    if (m_opaque_ptr)
    {
        const InlineFunctionInfo* inlined_info = m_opaque_ptr->GetInlinedFunctionInfo ();
        if (inlined_info)
            return inlined_info->GetCallSite().GetLine();
    }
    return 0;
}

uint32_t
SBBlock::GetInlinedCallSiteColumn () const
{
    if (m_opaque_ptr)
    {
        const InlineFunctionInfo* inlined_info = m_opaque_ptr->GetInlinedFunctionInfo ();
        if (inlined_info)
            return inlined_info->GetCallSite().GetColumn();
    }
    return 0;
}

void
SBBlock::AppendVariables (bool can_create, bool get_parent_variables, lldb_private::VariableList *var_list)
{
    if (IsValid())
    {
        bool show_inline = true;
        m_opaque_ptr->AppendVariables (can_create, get_parent_variables, show_inline, var_list);
    }
}

SBBlock
SBBlock::GetParent ()
{
    SBBlock sb_block;
    if (m_opaque_ptr)
        sb_block.m_opaque_ptr = m_opaque_ptr->GetParent();
    return sb_block;
}

lldb::SBBlock
SBBlock::GetContainingInlinedBlock  ()
{
    SBBlock sb_block;
    if (m_opaque_ptr)
        sb_block.m_opaque_ptr = m_opaque_ptr->GetContainingInlinedBlock ();
    return sb_block;
}

SBBlock
SBBlock::GetSibling ()
{
    SBBlock sb_block;
    if (m_opaque_ptr)
        sb_block.m_opaque_ptr = m_opaque_ptr->GetSibling();
    return sb_block;
}

SBBlock
SBBlock::GetFirstChild ()
{
    SBBlock sb_block;
    if (m_opaque_ptr)
        sb_block.m_opaque_ptr = m_opaque_ptr->GetFirstChild();
    return sb_block;
}

lldb_private::Block *
SBBlock::get ()
{
    return m_opaque_ptr;
}

void
SBBlock::reset (lldb_private::Block *block)
{
    m_opaque_ptr = block;
}

bool
SBBlock::GetDescription (SBStream &description)
{
    if (m_opaque_ptr)
    {
        lldb::user_id_t id = m_opaque_ptr->GetID();
        description.Printf ("Block: {id: %d} ", id);
        if (IsInlined())
        {
            description.Printf (" (inlined, '%s') ", GetInlinedName());
        }
        lldb_private::SymbolContext sc;
        m_opaque_ptr->CalculateSymbolContext (&sc);
        if (sc.function)
        {
            m_opaque_ptr->DumpAddressRanges (description.get(), 
                                             sc.function->GetAddressRange().GetBaseAddress().GetFileAddress());
        }
    }
    else
        description.Printf ("No value");
    
    return true;
}

uint32_t
SBBlock::GetNumRanges ()
{
    if (m_opaque_ptr)
        return m_opaque_ptr->GetNumRanges();
    return 0;
}

lldb::SBAddress
SBBlock::GetRangeStartAddress (uint32_t idx)
{
    lldb::SBAddress sb_addr;
    if (m_opaque_ptr)
    {
        AddressRange range;
        if (m_opaque_ptr->GetRangeAtIndex(idx, range))
        {
            sb_addr.ref() = range.GetBaseAddress();
        }
    }
    return sb_addr;
}

lldb::SBAddress
SBBlock::GetRangeEndAddress (uint32_t idx)
{
    lldb::SBAddress sb_addr;
    if (m_opaque_ptr)
    {
        AddressRange range;
        if (m_opaque_ptr->GetRangeAtIndex(idx, range))
        {
            sb_addr.ref() = range.GetBaseAddress();
            sb_addr.ref().Slide(range.GetByteSize());
        }
    }
    return sb_addr;
}

uint32_t
SBBlock::GetRangeIndexForBlockAddress (lldb::SBAddress block_addr)
{
    if (m_opaque_ptr && block_addr.IsValid())
    {
        uint32_t range_idx = UINT32_MAX;
        AddressRange range;
        if (m_opaque_ptr->GetRangeContainingAddress (block_addr.ref(), range, &range_idx))
        {
            return range_idx;
        }
    }

    return UINT32_MAX;
}

