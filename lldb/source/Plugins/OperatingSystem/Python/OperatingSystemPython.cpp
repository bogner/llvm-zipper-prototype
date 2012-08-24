//===-- OperatingSystemPython.cpp --------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
#ifndef LLDB_DISABLE_PYTHON

#include "OperatingSystemPython.h"
// C Includes
// C++ Includes
// Other libraries and framework includes
#include "llvm/ADT/Triple.h"

#include "lldb/Core/ArchSpec.h"
#include "lldb/Core/DataBufferHeap.h"
#include "lldb/Core/Debugger.h"
#include "lldb/Core/Module.h"
#include "lldb/Core/PluginManager.h"
#include "lldb/Core/RegisterValue.h"
#include "lldb/Core/ValueObjectVariable.h"
#include "lldb/Interpreter/CommandInterpreter.h"
#include "lldb/Interpreter/PythonDataObjects.h"
#include "lldb/Symbol/ClangNamespaceDecl.h"
#include "lldb/Symbol/ObjectFile.h"
#include "lldb/Symbol/VariableList.h"
#include "lldb/Target/Process.h"
#include "lldb/Target/StopInfo.h"
#include "lldb/Target/Target.h"
#include "lldb/Target/ThreadList.h"
#include "lldb/Target/Thread.h"
#include "Plugins/Process/Utility/DynamicRegisterInfo.h"
#include "Plugins/Process/Utility/RegisterContextMemory.h"
#include "Plugins/Process/Utility/ThreadMemory.h"

using namespace lldb;
using namespace lldb_private;

void
OperatingSystemPython::Initialize()
{
    PluginManager::RegisterPlugin (GetPluginNameStatic(),
                                   GetPluginDescriptionStatic(),
                                   CreateInstance);
}

void
OperatingSystemPython::Terminate()
{
    PluginManager::UnregisterPlugin (CreateInstance);
}

OperatingSystem *
OperatingSystemPython::CreateInstance (Process *process, bool force)
{
    // Python OperatingSystem plug-ins must be requested by name, so force must be true
    //if (force)
        return new OperatingSystemPython (process);
    return NULL;
}


const char *
OperatingSystemPython::GetPluginNameStatic()
{
    return "python";
}

const char *
OperatingSystemPython::GetPluginDescriptionStatic()
{
    return "Operating system plug-in that gathers OS information from a python class that implements the necessary OperatingSystem functionality.";
}


OperatingSystemPython::OperatingSystemPython (lldb_private::Process *process) :
    OperatingSystem (process),
    m_thread_list_valobj_sp (),
    m_register_info_ap (),
    m_interpreter(NULL),
    m_python_object(NULL)
{
    if (!process)
        return;
    lldb::TargetSP target_sp = process->CalculateTarget();
    if (!target_sp)
        return;
    m_interpreter = target_sp->GetDebugger().GetCommandInterpreter().GetScriptInterpreter();
    if (m_interpreter)
    {
        // TODO: hardcoded is not good
        auto object_sp = m_interpreter->CreateOSPlugin("operating_system.PlugIn",process->CalculateProcess());
        if (object_sp)
            m_python_object = object_sp->GetObject();
    }
}

OperatingSystemPython::~OperatingSystemPython ()
{
}

DynamicRegisterInfo *
OperatingSystemPython::GetDynamicRegisterInfo ()
{
    if (!m_interpreter || !m_python_object)
        return NULL;
    auto object_sp = m_interpreter->OSPlugin_QueryForRegisterInfo(m_interpreter->MakeScriptObject(m_python_object));
    if (!object_sp)
        return NULL;
    PythonDataObject dictionary_data_obj((PyObject*)object_sp->GetObject());
    PythonDataDictionary dictionary = dictionary_data_obj.GetDictionaryObject();
    if(!dictionary)
        return NULL;
    
    // TODO: iterate over the dictionary
    if (m_register_info_ap.get() == NULL && m_thread_list_valobj_sp)
    {
//        static ConstString g_gpr_member_name("gpr");
//        m_register_info_ap.reset (new DynamicRegisterInfo());
//        ConstString empty_name;
//        const bool can_create = true;
//        AddressType addr_type;
//        addr_t base_addr = LLDB_INVALID_ADDRESS;
//        ValueObjectSP gpr_valobj_sp (m_thread_list_valobj_sp->GetChildMemberWithName(GetThreadGPRMemberName (), can_create));
//        
//        if (gpr_valobj_sp->IsPointerType ())
//            base_addr = gpr_valobj_sp->GetPointerValue (&addr_type);
//        else
//            base_addr = gpr_valobj_sp->GetAddressOf (true, &addr_type);
//
//        ValueObjectSP child_valobj_sp;
//        if (gpr_valobj_sp)
//        {
//            ABI *abi = m_process->GetABI().get();
//            assert (abi);
//            uint32_t num_children = gpr_valobj_sp->GetNumChildren();
//            
//            ConstString gpr_name (gpr_valobj_sp->GetName());
//            uint32_t reg_num = 0;
//            for (uint32_t i=0; i<num_children; ++i)
//            {
//                child_valobj_sp = gpr_valobj_sp->GetChildAtIndex(i, can_create);
//
//                ConstString reg_name(child_valobj_sp->GetName());
//                if (reg_name)
//                {
//                    const char *reg_name_cstr = reg_name.GetCString();
//                    while (reg_name_cstr[0] == '_')
//                        ++reg_name_cstr;
//                    if (reg_name_cstr != reg_name.GetCString())
//                        reg_name.SetCString (reg_name_cstr);
//                }
//                
//                RegisterInfo reg_info;
//                if (abi->GetRegisterInfoByName(reg_name, reg_info))
//                {
//                    // Adjust the byte size and the offset to match the layout of registers in our struct
//                    reg_info.byte_size = child_valobj_sp->GetByteSize();
//                    reg_info.byte_offset = child_valobj_sp->GetAddressOf(true, &addr_type) - base_addr;
//                    reg_info.kinds[eRegisterKindLLDB] = reg_num++;
//                    m_register_info_ap->AddRegister (reg_info, reg_name, empty_name, gpr_name);
//                }
//                else
//                {
//                    printf ("not able to find register info for %s\n", reg_name.GetCString()); // REMOVE THIS printf before checkin!!!
//                }
//            }
//            
//            m_register_info_ap->Finalize();
//        }
    }
    assert (m_register_info_ap.get());
    return m_register_info_ap.get();
}

//------------------------------------------------------------------
// PluginInterface protocol
//------------------------------------------------------------------
const char *
OperatingSystemPython::GetPluginName()
{
    return "OperatingSystemPython";
}

const char *
OperatingSystemPython::GetShortPluginName()
{
    return GetPluginNameStatic();
}

uint32_t
OperatingSystemPython::GetPluginVersion()
{
    return 1;
}

bool
OperatingSystemPython::UpdateThreadList (ThreadList &old_thread_list, ThreadList &new_thread_list)
{
    
    if (!m_interpreter || !m_python_object)
        return NULL;
    auto object_sp = m_interpreter->OSPlugin_QueryForThreadsInfo(m_interpreter->MakeScriptObject(m_python_object));
    if (!object_sp)
        return NULL;
    PythonDataObject dictionary_data_obj((PyObject*)object_sp->GetObject());
    PythonDataDictionary dictionary = dictionary_data_obj.GetDictionaryObject();
    if(!dictionary)
        return NULL;
    
    // TODO: read from the dict
    
    // and parse the returned dictionary. We need to pass in the a Dictionary
    // with the same kind of info we want back so we can reuse old threads, but
    // only create new ones.
    
    // Make any constant strings once and cache the uniqued C string values
    // so we don't have to rehash them each time through this function call
//    dict thread_info_dict = python.get_thread_info()
//    for thread_info in thread_info_dict:
//    {
//        ThreadSP thread_sp (old_thread_list.FindThreadByID (tid, false));
//        if (!thread_sp)
//            thread_sp.reset (new ThreadMemory (m_process->shared_from_this(), tid, valobj_sp));
//        new_thread_list.AddThread(thread_sp);
//    }
    new_thread_list = old_thread_list;
    return new_thread_list.GetSize(false) > 0;
}

void
OperatingSystemPython::ThreadWasSelected (Thread *thread)
{
}

RegisterContextSP
OperatingSystemPython::CreateRegisterContextForThread (Thread *thread)
{

    if (!m_interpreter || !m_python_object || !thread)
        return NULL;
    auto object_sp = m_interpreter->OSPlugin_QueryForThreadInfo(m_interpreter->MakeScriptObject(m_python_object),
                                                                thread->GetID());
    if (!object_sp)
        return NULL;
    PythonDataObject pack_info_data_obj((PyObject*)object_sp->GetObject());
    if(!pack_info_data_obj)
        return NULL;

    RegisterContextSP reg_ctx_sp;
//    bytes b = get_register_context_data(thread)
//    if (b)
//    {
//        reg_ctx_sp.reset (new RegisterContextMemory (*thread, 0, *GetDynamicRegisterInfo (), base_addr));
//        // set bytes
//    }
    return reg_ctx_sp;
}

StopInfoSP
OperatingSystemPython::CreateThreadStopReason (lldb_private::Thread *thread)
{
    // We should have gotten the thread stop info from the dictionary of data for
    // the thread in the initial call to get_thread_info(), this should have been
    // cached so we can return it here
    StopInfoSP stop_info_sp; //(StopInfo::CreateStopReasonWithSignal (*thread, SIGSTOP));
    return stop_info_sp;
}


#endif // #ifndef LLDB_DISABLE_PYTHON
