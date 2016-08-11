//===-- InstrumentationRuntime.h --------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef liblldb_InstrumentationRuntime_h_
#define liblldb_InstrumentationRuntime_h_

// C Includes
// C++ Includes
#include <vector>
#include <map>

// Other libraries and framework includes
// Project includes
#include "lldb/lldb-forward.h"
#include "lldb/lldb-private.h"
#include "lldb/lldb-types.h"
#include "lldb/Core/PluginInterface.h"
#include "lldb/Core/StructuredData.h"

namespace lldb_private {
    
typedef std::map<lldb::InstrumentationRuntimeType, lldb::InstrumentationRuntimeSP> InstrumentationRuntimeCollection;
    
class InstrumentationRuntime :
    public std::enable_shared_from_this<InstrumentationRuntime>,
    public PluginInterface
{
    /// The instrumented process.
    lldb::ProcessWP m_process_wp;

    /// The module containing the instrumentation runtime.
    lldb::ModuleSP m_runtime_module;

    /// The breakpoint in the instrumentation runtime.
    lldb::user_id_t m_breakpoint_id;

    /// Indicates whether or not breakpoints have been registered in the instrumentation runtime.
    bool m_is_active;

protected:
    InstrumentationRuntime(const lldb::ProcessSP &process_sp)
        : m_process_wp(), m_runtime_module(), m_breakpoint_id(0), m_is_active(false)
    {
        if (process_sp)
            m_process_wp = process_sp;
    }

    lldb::ProcessSP
    GetProcessSP()
    {
        return m_process_wp.lock();
    }

    lldb::ModuleSP
    GetRuntimeModuleSP()
    {
        return m_runtime_module;
    }

    void
    SetRuntimeModuleSP(lldb::ModuleSP module_sp)
    {
        m_runtime_module = module_sp;
    }

    lldb::user_id_t
    GetBreakpointID() const
    {
        return m_breakpoint_id;
    }

    void
    SetBreakpointID(lldb::user_id_t ID)
    {
        m_breakpoint_id = ID;
    }

    void
    SetActive(bool IsActive)
    {
        m_is_active = IsActive;
    }

public:
    
    static void
    ModulesDidLoad(lldb_private::ModuleList &module_list, Process *process, InstrumentationRuntimeCollection &runtimes);

    /// Look for the instrumentation runtime in \p module_list. Register and activate the runtime if this hasn't already
    /// been done.
    virtual void
    ModulesDidLoad(lldb_private::ModuleList &module_list) = 0;

    bool
    IsActive() const
    {
        return m_is_active;
    }

    virtual lldb::ThreadCollectionSP
    GetBacktracesFromExtendedStopInfo(StructuredData::ObjectSP info);
    
};
    
} // namespace lldb_private

#endif  // liblldb_InstrumentationRuntime_h_
