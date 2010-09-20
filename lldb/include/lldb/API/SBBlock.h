//===-- SBBlock.h -----------------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_SBBlock_h_
#define LLDB_SBBlock_h_

#include "lldb/API/SBDefines.h"

namespace lldb {

class SBBlock
{
public:

    SBBlock ();

    ~SBBlock ();

    bool
    IsInlined () const;

    bool
    IsValid () const;

    const char *
    GetInlinedName () const;

    lldb::SBFileSpec
    GetInlinedCallSiteFile () const;

    uint32_t 
    GetInlinedCallSiteLine () const;

    uint32_t
    GetInlinedCallSiteColumn () const;

    lldb::SBBlock
    GetParent ();
    
    lldb::SBBlock
    GetSibling ();
    
    lldb::SBBlock
    GetFirstChild ();

    bool
    GetDescription (lldb::SBStream &description);

    // The following function gets called by Python when a user tries to print
    // an object of this class.  It takes no arguments and returns a
    // PyObject *, which contains a char * (and it must be named "__repr__");

    PyObject *
    __repr__ ();

private:
    friend class SBFrame;
    friend class SBSymbolContext;

#ifndef SWIG

    SBBlock (lldb_private::Block *lldb_object_ptr);

    void
    AppendVariables (bool can_create, bool get_parent_variables, lldb_private::VariableList *var_list);

#endif

    lldb_private::Block *m_opaque_ptr;
};


} // namespace lldb

#endif // LLDB_SBBlock_h_
