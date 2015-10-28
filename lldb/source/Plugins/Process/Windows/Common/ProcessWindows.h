//===-- ProcessWindows.h ----------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef liblldb_Plugins_Process_Windows_Common_ProcessWindows_H_
#define liblldb_Plugins_Process_Windows_Common_ProcessWindows_H_

// Other libraries and framework includes
#include "lldb/lldb-forward.h"
#include "lldb/Core/Error.h"
#include "lldb/Target/Process.h"

namespace lldb_private
{

class ProcessWindows : public lldb_private::Process
{
public:
    //------------------------------------------------------------------
    // Constructors and destructors
    //------------------------------------------------------------------
    ProcessWindows(lldb::TargetSP target_sp,
                   lldb_private::Listener &listener);

    ~ProcessWindows();

    size_t GetSTDOUT(char *buf, size_t buf_size, lldb_private::Error &error) override;
    size_t GetSTDERR(char *buf, size_t buf_size, lldb_private::Error &error) override;
    size_t PutSTDIN(const char *buf, size_t buf_size, lldb_private::Error &error) override;

    lldb::addr_t GetImageInfoAddress() override;
};

}

#endif  // liblldb_Plugins_Process_Windows_Common_ProcessWindows_H_
