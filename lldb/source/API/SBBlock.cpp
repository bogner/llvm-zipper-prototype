//===-- SBBlock.cpp ---------------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "lldb/API/SBBlock.h"
#include "lldb/API/SBFileSpec.h"
#include "lldb/Symbol/Block.h"
#include "lldb/Symbol/Function.h"

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



