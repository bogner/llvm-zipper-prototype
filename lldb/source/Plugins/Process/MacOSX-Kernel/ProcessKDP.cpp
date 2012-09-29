//===-- ProcessKDP.cpp ------------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

// C Includes
#include <errno.h>
#include <stdlib.h>

// C++ Includes
// Other libraries and framework includes
#include "lldb/Core/ConnectionFileDescriptor.h"
#include "lldb/Core/Debugger.h"
#include "lldb/Core/PluginManager.h"
#include "lldb/Core/Module.h"
#include "lldb/Core/ModuleSpec.h"
#include "lldb/Core/State.h"
#include "lldb/Core/UUID.h"
#include "lldb/Host/Host.h"
#include "lldb/Host/Symbols.h"
#include "lldb/Symbol/ObjectFile.h"
#include "lldb/Target/RegisterContext.h"
#include "lldb/Target/Target.h"
#include "lldb/Target/Thread.h"

// Project includes
#include "ProcessKDP.h"
#include "ProcessKDPLog.h"
#include "ThreadKDP.h"

using namespace lldb;
using namespace lldb_private;

const char *
ProcessKDP::GetPluginNameStatic()
{
    return "kdp-remote";
}

const char *
ProcessKDP::GetPluginDescriptionStatic()
{
    return "KDP Remote protocol based debugging plug-in for darwin kernel debugging.";
}

void
ProcessKDP::Terminate()
{
    PluginManager::UnregisterPlugin (ProcessKDP::CreateInstance);
}


lldb::ProcessSP
ProcessKDP::CreateInstance (Target &target, 
                            Listener &listener,
                            const FileSpec *crash_file_path)
{
    lldb::ProcessSP process_sp;
    if (crash_file_path == NULL)
        process_sp.reset(new ProcessKDP (target, listener));
    return process_sp;
}

bool
ProcessKDP::CanDebug(Target &target, bool plugin_specified_by_name)
{
    if (plugin_specified_by_name)
        return true;

    // For now we are just making sure the file exists for a given module
    Module *exe_module = target.GetExecutableModulePointer();
    if (exe_module)
    {
        const llvm::Triple &triple_ref = target.GetArchitecture().GetTriple();
        switch (triple_ref.getOS())
        {
            case llvm::Triple::Darwin:  // Should use "macosx" for desktop and "ios" for iOS, but accept darwin just in case
            case llvm::Triple::MacOSX:  // For desktop targets
            case llvm::Triple::IOS:     // For arm targets
                if (triple_ref.getVendor() == llvm::Triple::Apple)
                {
                    ObjectFile *exe_objfile = exe_module->GetObjectFile();
                    if (exe_objfile->GetType() == ObjectFile::eTypeExecutable && 
                        exe_objfile->GetStrata() == ObjectFile::eStrataKernel)
                        return true;
                }
                break;

            default:
                break;
        }
    }
    return false;
}

//----------------------------------------------------------------------
// ProcessKDP constructor
//----------------------------------------------------------------------
ProcessKDP::ProcessKDP(Target& target, Listener &listener) :
    Process (target, listener),
    m_comm("lldb.process.kdp-remote.communication"),
    m_async_broadcaster (NULL, "lldb.process.kdp-remote.async-broadcaster"),
    m_async_thread (LLDB_INVALID_HOST_THREAD),
    m_destroy_in_process (false)
{
    m_async_broadcaster.SetEventName (eBroadcastBitAsyncThreadShouldExit,   "async thread should exit");
    m_async_broadcaster.SetEventName (eBroadcastBitAsyncContinue,           "async thread continue");
}

//----------------------------------------------------------------------
// Destructor
//----------------------------------------------------------------------
ProcessKDP::~ProcessKDP()
{
    Clear();
    // We need to call finalize on the process before destroying ourselves
    // to make sure all of the broadcaster cleanup goes as planned. If we
    // destruct this class, then Process::~Process() might have problems
    // trying to fully destroy the broadcaster.
    Finalize();
}

//----------------------------------------------------------------------
// PluginInterface
//----------------------------------------------------------------------
const char *
ProcessKDP::GetPluginName()
{
    return "Process debugging plug-in that uses the Darwin KDP remote protocol";
}

const char *
ProcessKDP::GetShortPluginName()
{
    return GetPluginNameStatic();
}

uint32_t
ProcessKDP::GetPluginVersion()
{
    return 1;
}

Error
ProcessKDP::WillLaunch (Module* module)
{
    Error error;
    error.SetErrorString ("launching not supported in kdp-remote plug-in");
    return error;
}

Error
ProcessKDP::WillAttachToProcessWithID (lldb::pid_t pid)
{
    Error error;
    error.SetErrorString ("attaching to a by process ID not supported in kdp-remote plug-in");
    return error;
}

Error
ProcessKDP::WillAttachToProcessWithName (const char *process_name, bool wait_for_launch)
{
    Error error;
    error.SetErrorString ("attaching to a by process name not supported in kdp-remote plug-in");
    return error;
}

Error
ProcessKDP::DoConnectRemote (Stream *strm, const char *remote_url)
{
    Error error;

    // Don't let any JIT happen when doing KDP as we can't allocate
    // memory and we don't want to be mucking with threads that might
    // already be handling exceptions
    SetCanJIT(false);

    if (remote_url == NULL || remote_url[0] == '\0')
    {
        error.SetErrorStringWithFormat ("invalid connection URL '%s'", remote_url);
        return error;
    }

    std::auto_ptr<ConnectionFileDescriptor> conn_ap(new ConnectionFileDescriptor());
    if (conn_ap.get())
    {
        // Only try once for now.
        // TODO: check if we should be retrying?
        const uint32_t max_retry_count = 1;
        for (uint32_t retry_count = 0; retry_count < max_retry_count; ++retry_count)
        {
            if (conn_ap->Connect(remote_url, &error) == eConnectionStatusSuccess)
                break;
            usleep (100000);
        }
    }

    if (conn_ap->IsConnected())
    {
        const uint16_t reply_port = conn_ap->GetReadPort ();

        if (reply_port != 0)
        {
            m_comm.SetConnection(conn_ap.release());

            if (m_comm.SendRequestReattach(reply_port))
            {
                if (m_comm.SendRequestConnect(reply_port, reply_port, "Greetings from LLDB..."))
                {
                    m_comm.GetVersion();
                    uint32_t cpu = m_comm.GetCPUType();
                    uint32_t sub = m_comm.GetCPUSubtype();
                    ArchSpec kernel_arch;
                    kernel_arch.SetArchitecture(eArchTypeMachO, cpu, sub);
                    m_target.SetArchitecture(kernel_arch);

                    /* Get the kernel's UUID and load address via kdp-kernelversion packet.  */
                        
                    UUID kernel_uuid = m_comm.GetUUID ();
                    addr_t kernel_load_addr = m_comm.GetLoadAddress ();
                    if (strm)
                    {
                        char uuidbuf[64];
                        strm->Printf ("Kernel UUID: %s\n", kernel_uuid.GetAsCString (uuidbuf, sizeof (uuidbuf)));
                        strm->Printf ("Load Address: 0x%llx\n", kernel_load_addr);
                        strm->Flush ();
                    }

                    /* Set the kernel's LoadAddress based on the information from kdp.
                       This would normally be handled by the DynamicLoaderDarwinKernel plugin but there's no easy
                       way to communicate the UUID / load addr from kdp back up to that plugin so we'll set it here. */
                    ModuleSP exe_module_sp = m_target.GetExecutableModule ();
                    bool find_and_load_kernel = true;
                    if (exe_module_sp.get ())
                    {
                        ObjectFile *exe_objfile = exe_module_sp->GetObjectFile();
                        if (exe_objfile->GetType() == ObjectFile::eTypeExecutable && 
                            exe_objfile->GetStrata() == ObjectFile::eStrataKernel)
                        {
                            UUID exe_objfile_uuid;
                            if (exe_objfile->GetUUID (&exe_objfile_uuid) && kernel_uuid == exe_objfile_uuid
                                && exe_objfile->GetHeaderAddress().IsValid())
                            {
                                find_and_load_kernel = false;
                                addr_t slide = kernel_load_addr - exe_objfile->GetHeaderAddress().GetFileAddress();
                                if (slide != 0)
                                {
                                    bool changed = false;
                                    exe_module_sp->SetLoadAddress (m_target, slide, changed);
                                    if (changed)
                                    {
                                        ModuleList modlist;
                                        modlist.Append (exe_module_sp);
                                        m_target.ModulesDidLoad (modlist);
                                    }
                                }
                            }
                        }
                    }

                    // If the executable binary is not the same as the kernel being run on the remote host,
                    // see if Symbols::DownloadObjectAndSymbolFile can find us a symbol file based on the UUID
                    // and if so, load it at the correct address.
                    if (find_and_load_kernel && kernel_load_addr != LLDB_INVALID_ADDRESS && kernel_uuid.IsValid())
                    {
                        ModuleSpec sym_spec;
                        sym_spec.GetUUID() = kernel_uuid;
                        if (Symbols::DownloadObjectAndSymbolFile (sym_spec) 
                            && sym_spec.GetArchitecture().IsValid() 
                            && sym_spec.GetSymbolFileSpec().Exists())
                        {
                            ModuleSP kernel_sp = m_target.GetSharedModule (sym_spec);
                            if (kernel_sp.get())
                            {
                                m_target.SetExecutableModule(kernel_sp, false);
                                if (kernel_sp->GetObjectFile() && kernel_sp->GetObjectFile()->GetHeaderAddress().IsValid())
                                {
                                    addr_t slide = kernel_load_addr - kernel_sp->GetObjectFile()->GetHeaderAddress().GetFileAddress();
                                    bool changed = false;
                                    kernel_sp->SetLoadAddress (m_target, slide, changed);
                                    if (changed)
                                    {
                                        ModuleList modlist;
                                        modlist.Append (kernel_sp);
                                        m_target.ModulesDidLoad (modlist);
                                    }
                                    if (strm)
                                    {
                                        strm->Printf ("Loaded kernel file %s/%s\n", 
                                                      kernel_sp->GetFileSpec().GetDirectory().AsCString(),
                                                      kernel_sp->GetFileSpec().GetFilename().AsCString());
                                        strm->Flush ();
                                    }
                                }
                            }
                        }
                    }


                    // Set the thread ID
                    UpdateThreadListIfNeeded ();
                    SetID (1);
                    GetThreadList ();
                    SetPrivateState (eStateStopped);
                    StreamSP async_strm_sp(m_target.GetDebugger().GetAsyncOutputStream());
                    if (async_strm_sp)
                    {
                        const char *cstr;
                        if ((cstr = m_comm.GetKernelVersion ()) != NULL)
                        {
                            async_strm_sp->Printf ("Version: %s\n", cstr);
                            async_strm_sp->Flush();
                        }
//                      if ((cstr = m_comm.GetImagePath ()) != NULL)
//                      {
//                          async_strm_sp->Printf ("Image Path: %s\n", cstr);
//                          async_strm_sp->Flush();
//                      }            
                    }
                }
                else
                {
                    puts ("KDP_CONNECT failed"); // REMOVE THIS
                    error.SetErrorString("KDP_REATTACH failed");
                }
            }
            else
            {
                puts ("KDP_REATTACH failed"); // REMOVE THIS
                error.SetErrorString("KDP_REATTACH failed");
            }
        }
        else
        {
            error.SetErrorString("invalid reply port from UDP connection");
        }
    }
    else
    {
        if (error.Success())
            error.SetErrorStringWithFormat ("failed to connect to '%s'", remote_url);
    }
    if (error.Fail())
        m_comm.Disconnect();

    return error;
}

//----------------------------------------------------------------------
// Process Control
//----------------------------------------------------------------------
Error
ProcessKDP::DoLaunch (Module *exe_module, 
                      const ProcessLaunchInfo &launch_info)
{
    Error error;
    error.SetErrorString ("launching not supported in kdp-remote plug-in");
    return error;
}


Error
ProcessKDP::DoAttachToProcessWithID (lldb::pid_t attach_pid)
{
    Error error;
    error.SetErrorString ("attach to process by ID is not suppported in kdp remote debugging");
    return error;
}

Error
ProcessKDP::DoAttachToProcessWithID (lldb::pid_t attach_pid, const ProcessAttachInfo &attach_info)
{
    Error error;
    error.SetErrorString ("attach to process by ID is not suppported in kdp remote debugging");
    return error;
}

Error
ProcessKDP::DoAttachToProcessWithName (const char *process_name, bool wait_for_launch, const ProcessAttachInfo &attach_info)
{
    Error error;
    error.SetErrorString ("attach to process by name is not suppported in kdp remote debugging");
    return error;
}


void
ProcessKDP::DidAttach ()
{
    LogSP log (ProcessKDPLog::GetLogIfAllCategoriesSet (KDP_LOG_PROCESS));
    if (log)
        log->Printf ("ProcessKDP::DidAttach()");
    if (GetID() != LLDB_INVALID_PROCESS_ID)
    {
        // TODO: figure out the register context that we will use
    }
}

Error
ProcessKDP::WillResume ()
{
    return Error();
}

Error
ProcessKDP::DoResume ()
{
    Error error;
    LogSP log (ProcessKDPLog::GetLogIfAllCategoriesSet (KDP_LOG_PROCESS));
    // Only start the async thread if we try to do any process control
    if (!IS_VALID_LLDB_HOST_THREAD(m_async_thread))
        StartAsyncThread ();

    bool resume = false;
    
    // With KDP there is only one thread we can tell what to do
    ThreadSP kernel_thread_sp (GetKernelThread(m_thread_list, m_thread_list));
    if (kernel_thread_sp)
    {
        const StateType thread_resume_state = kernel_thread_sp->GetTemporaryResumeState();
        switch (thread_resume_state)
        {
            case eStateSuspended:
                // Nothing to do here when a thread will stay suspended
                // we just leave the CPU mask bit set to zero for the thread
                puts("REMOVE THIS: ProcessKDP::DoResume () -- thread suspended");
                break;
                
            case eStateStepping:
                puts("REMOVE THIS: ProcessKDP::DoResume () -- thread stepping");
                kernel_thread_sp->GetRegisterContext()->HardwareSingleStep (true);
                resume = true;
                break;
    
            case eStateRunning:
                puts("REMOVE THIS: ProcessKDP::DoResume () -- thread running");
                kernel_thread_sp->GetRegisterContext()->HardwareSingleStep (false);
                resume = true;
                break;

            default:
                // The only valid thread resume states are listed above
                assert (!"invalid thread resume state");
                break;
        }
    }

    if (resume)
    {
        if (log)
            log->Printf ("ProcessKDP::DoResume () sending resume");
        
        if (m_comm.SendRequestResume ())
        {
            m_async_broadcaster.BroadcastEvent (eBroadcastBitAsyncContinue);
            SetPrivateState(eStateRunning);
        }
        else
            error.SetErrorString ("KDP resume failed");
    }
    else
    {
        error.SetErrorString ("kernel thread is suspended");        
    }
    
    return error;
}

lldb::ThreadSP
ProcessKDP::GetKernelThread(ThreadList &old_thread_list, ThreadList &new_thread_list)
{
    // KDP only tells us about one thread/core. Any other threads will usually
    // be the ones that are read from memory by the OS plug-ins.
    const lldb::tid_t kernel_tid = 1;
    ThreadSP thread_sp (old_thread_list.FindThreadByID (kernel_tid, false));
    if (!thread_sp)
    {
        thread_sp.reset(new ThreadKDP (shared_from_this(), kernel_tid));
        new_thread_list.AddThread(thread_sp);
    }
    return thread_sp;
}




bool
ProcessKDP::UpdateThreadList (ThreadList &old_thread_list, ThreadList &new_thread_list)
{
    // locker will keep a mutex locked until it goes out of scope
    LogSP log (ProcessKDPLog::GetLogIfAllCategoriesSet (KDP_LOG_THREAD));
    if (log && log->GetMask().Test(KDP_LOG_VERBOSE))
        log->Printf ("ProcessKDP::%s (pid = %llu)", __FUNCTION__, GetID());
    
    // Even though there is a CPU mask, it doesn't mean to can see each CPU
    // indivudually, there is really only one. Lets call this thread 1.
    GetKernelThread (old_thread_list, new_thread_list);

    return new_thread_list.GetSize(false) > 0;
}

void
ProcessKDP::RefreshStateAfterStop ()
{
    // Let all threads recover from stopping and do any clean up based
    // on the previous thread state (if any).
    m_thread_list.RefreshStateAfterStop();
}

Error
ProcessKDP::DoHalt (bool &caused_stop)
{
    Error error;
    
    if (m_comm.IsRunning())
    {
        if (m_destroy_in_process)
        {
            // If we are attemping to destroy, we need to not return an error to
            // Halt or DoDestroy won't get called.
            // We are also currently running, so send a process stopped event
            SetPrivateState (eStateStopped);
        }
        else
        {
            error.SetErrorString ("KDP cannot interrupt a running kernel");
        }
    }
    return error;
}

Error
ProcessKDP::DoDetach()
{
    Error error;
    LogSP log (ProcessKDPLog::GetLogIfAllCategoriesSet(KDP_LOG_PROCESS));
    if (log)
        log->Printf ("ProcessKDP::DoDetach()");
    
    if (m_comm.IsRunning())
    {
        // We are running and we can't interrupt a running kernel, so we need
        // to just close the connection to the kernel and hope for the best
    }
    else
    {
        DisableAllBreakpointSites ();
        
        m_thread_list.DiscardThreadPlans();
        
        if (m_comm.IsConnected())
        {

            m_comm.SendRequestDisconnect();

            size_t response_size = m_comm.Disconnect ();
            if (log)
            {
                if (response_size)
                    log->PutCString ("ProcessKDP::DoDetach() detach packet sent successfully");
                else
                    log->PutCString ("ProcessKDP::DoDetach() detach packet send failed");
            }
        }
    }
    StopAsyncThread ();    
    m_comm.Clear();
    
    SetPrivateState (eStateDetached);
    ResumePrivateStateThread();
    
    //KillDebugserverProcess ();
    return error;
}

Error
ProcessKDP::WillDestroy ()
{
    Error error;
    m_destroy_in_process = true;
    return error;
}

Error
ProcessKDP::DoDestroy ()
{
    // For KDP there really is no difference between destroy and detach
    return DoDetach();
}

//------------------------------------------------------------------
// Process Queries
//------------------------------------------------------------------

bool
ProcessKDP::IsAlive ()
{
    return m_comm.IsConnected() && m_private_state.GetValue() != eStateExited;
}

//------------------------------------------------------------------
// Process Memory
//------------------------------------------------------------------
size_t
ProcessKDP::DoReadMemory (addr_t addr, void *buf, size_t size, Error &error)
{
    if (m_comm.IsConnected())
        return m_comm.SendRequestReadMemory (addr, buf, size, error);
    error.SetErrorString ("not connected");
    return 0;
}

size_t
ProcessKDP::DoWriteMemory (addr_t addr, const void *buf, size_t size, Error &error)
{
    if (m_comm.IsConnected())
        return m_comm.SendRequestWriteMemory (addr, buf, size, error);
    error.SetErrorString ("not connected");
    return 0;
}

lldb::addr_t
ProcessKDP::DoAllocateMemory (size_t size, uint32_t permissions, Error &error)
{
    error.SetErrorString ("memory allocation not suppported in kdp remote debugging");
    return LLDB_INVALID_ADDRESS;
}

Error
ProcessKDP::DoDeallocateMemory (lldb::addr_t addr)
{
    Error error;
    error.SetErrorString ("memory deallocation not suppported in kdp remote debugging");
    return error;
}

Error
ProcessKDP::EnableBreakpoint (BreakpointSite *bp_site)
{
    if (m_comm.LocalBreakpointsAreSupported ())
    {
        Error error;
        if (!bp_site->IsEnabled())
        {
            if (m_comm.SendRequestBreakpoint(true, bp_site->GetLoadAddress()))
            {
                bp_site->SetEnabled(true);
                bp_site->SetType (BreakpointSite::eExternal);
            }
            else
            {
                error.SetErrorString ("KDP set breakpoint failed");
            }
        }
        return error;
    }
    return EnableSoftwareBreakpoint (bp_site);
}

Error
ProcessKDP::DisableBreakpoint (BreakpointSite *bp_site)
{
    if (m_comm.LocalBreakpointsAreSupported ())
    {
        Error error;
        if (bp_site->IsEnabled())
        {
            BreakpointSite::Type bp_type = bp_site->GetType();
            if (bp_type == BreakpointSite::eExternal)
            {
                if (m_destroy_in_process && m_comm.IsRunning())
                {
                    // We are trying to destroy our connection and we are running
                    bp_site->SetEnabled(false);
                }
                else
                {
                    if (m_comm.SendRequestBreakpoint(false, bp_site->GetLoadAddress()))
                        bp_site->SetEnabled(false);
                    else
                        error.SetErrorString ("KDP remove breakpoint failed");
                }
            }
            else
            {
                error = DisableSoftwareBreakpoint (bp_site);
            }
        }
        return error;
    }
    return DisableSoftwareBreakpoint (bp_site);
}

Error
ProcessKDP::EnableWatchpoint (Watchpoint *wp)
{
    Error error;
    error.SetErrorString ("watchpoints are not suppported in kdp remote debugging");
    return error;
}

Error
ProcessKDP::DisableWatchpoint (Watchpoint *wp)
{
    Error error;
    error.SetErrorString ("watchpoints are not suppported in kdp remote debugging");
    return error;
}

void
ProcessKDP::Clear()
{
    m_thread_list.Clear();
}

Error
ProcessKDP::DoSignal (int signo)
{
    Error error;
    error.SetErrorString ("sending signals is not suppported in kdp remote debugging");
    return error;
}

void
ProcessKDP::Initialize()
{
    static bool g_initialized = false;
    
    if (g_initialized == false)
    {
        g_initialized = true;
        PluginManager::RegisterPlugin (GetPluginNameStatic(),
                                       GetPluginDescriptionStatic(),
                                       CreateInstance);
        
        Log::Callbacks log_callbacks = {
            ProcessKDPLog::DisableLog,
            ProcessKDPLog::EnableLog,
            ProcessKDPLog::ListLogCategories
        };
        
        Log::RegisterLogChannel (ProcessKDP::GetPluginNameStatic(), log_callbacks);
    }
}

bool
ProcessKDP::StartAsyncThread ()
{
    LogSP log (ProcessKDPLog::GetLogIfAllCategoriesSet(KDP_LOG_PROCESS));
    
    if (log)
        log->Printf ("ProcessKDP::StartAsyncThread ()");
    
    if (IS_VALID_LLDB_HOST_THREAD(m_async_thread))
        return true;

    m_async_thread = Host::ThreadCreate ("<lldb.process.kdp-remote.async>", ProcessKDP::AsyncThread, this, NULL);
    return IS_VALID_LLDB_HOST_THREAD(m_async_thread);
}

void
ProcessKDP::StopAsyncThread ()
{
    LogSP log (ProcessKDPLog::GetLogIfAllCategoriesSet(KDP_LOG_PROCESS));
    
    if (log)
        log->Printf ("ProcessKDP::StopAsyncThread ()");
    
    m_async_broadcaster.BroadcastEvent (eBroadcastBitAsyncThreadShouldExit);
    
    // Stop the stdio thread
    if (IS_VALID_LLDB_HOST_THREAD(m_async_thread))
    {
        Host::ThreadJoin (m_async_thread, NULL, NULL);
        m_async_thread = LLDB_INVALID_HOST_THREAD;
    }
}


void *
ProcessKDP::AsyncThread (void *arg)
{
    ProcessKDP *process = (ProcessKDP*) arg;
    
    const lldb::pid_t pid = process->GetID();

    LogSP log (ProcessKDPLog::GetLogIfAllCategoriesSet (KDP_LOG_PROCESS));
    if (log)
        log->Printf ("ProcessKDP::AsyncThread (arg = %p, pid = %llu) thread starting...", arg, pid);
    
    Listener listener ("ProcessKDP::AsyncThread");
    EventSP event_sp;
    const uint32_t desired_event_mask = eBroadcastBitAsyncContinue |
                                        eBroadcastBitAsyncThreadShouldExit;
    
    
    if (listener.StartListeningForEvents (&process->m_async_broadcaster, desired_event_mask) == desired_event_mask)
    {
        bool done = false;
        while (!done)
        {
            if (log)
                log->Printf ("ProcessKDP::AsyncThread (pid = %llu) listener.WaitForEvent (NULL, event_sp)...",
                             pid);
            if (listener.WaitForEvent (NULL, event_sp))
            {
                uint32_t event_type = event_sp->GetType();
                if (log)
                    log->Printf ("ProcessKDP::AsyncThread (pid = %llu) Got an event of type: %d...",
                                 pid,
                                 event_type);
                
                // When we are running, poll for 1 second to try and get an exception
                // to indicate the process has stopped. If we don't get one, check to
                // make sure no one asked us to exit
                bool is_running = false;
                DataExtractor exc_reply_packet;
                do
                {
                    switch (event_type)
                    {
                    case eBroadcastBitAsyncContinue:
                        {
                            is_running = true;
                            if (process->m_comm.WaitForPacketWithTimeoutMicroSeconds (exc_reply_packet, 1 * USEC_PER_SEC))
                            {
                                ThreadSP thread_sp (process->GetKernelThread(process->GetThreadList(), process->GetThreadList()));
                                thread_sp->GetRegisterContext()->InvalidateAllRegisters();
                                static_cast<ThreadKDP *>(thread_sp.get())->SetStopInfoFrom_KDP_EXCEPTION (exc_reply_packet);

                                // TODO: parse the stop reply packet
                                is_running = false;                                
                                process->SetPrivateState(eStateStopped);
                            }
                            else
                            {
                                // Check to see if we are supposed to exit. There is no way to
                                // interrupt a running kernel, so all we can do is wait for an
                                // exception or detach...
                                if (listener.GetNextEvent(event_sp))
                                {
                                    // We got an event, go through the loop again
                                    event_type = event_sp->GetType();
                                }
                            }
                        }
                        break;
                            
                    case eBroadcastBitAsyncThreadShouldExit:
                        if (log)
                            log->Printf ("ProcessKDP::AsyncThread (pid = %llu) got eBroadcastBitAsyncThreadShouldExit...",
                                         pid);
                        done = true;
                        is_running = false;
                        break;
                            
                    default:
                        if (log)
                            log->Printf ("ProcessKDP::AsyncThread (pid = %llu) got unknown event 0x%8.8x",
                                         pid,
                                         event_type);
                        done = true;
                        is_running = false;
                        break;
                    }
                } while (is_running);
            }
            else
            {
                if (log)
                    log->Printf ("ProcessKDP::AsyncThread (pid = %llu) listener.WaitForEvent (NULL, event_sp) => false",
                                 pid);
                done = true;
            }
        }
    }
    
    if (log)
        log->Printf ("ProcessKDP::AsyncThread (arg = %p, pid = %llu) thread exiting...",
                     arg,
                     pid);
    
    process->m_async_thread = LLDB_INVALID_HOST_THREAD;
    return NULL;
}


