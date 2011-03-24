//===-- Target.cpp ----------------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "lldb/Target/Target.h"

// C Includes
// C++ Includes
// Other libraries and framework includes
// Project includes
#include "lldb/Breakpoint/BreakpointResolver.h"
#include "lldb/Breakpoint/BreakpointResolverAddress.h"
#include "lldb/Breakpoint/BreakpointResolverFileLine.h"
#include "lldb/Breakpoint/BreakpointResolverName.h"
#include "lldb/Core/Debugger.h"
#include "lldb/Core/Event.h"
#include "lldb/Core/Log.h"
#include "lldb/Core/StreamString.h"
#include "lldb/Core/Timer.h"
#include "lldb/Core/ValueObject.h"
#include "lldb/Host/Host.h"
#include "lldb/Interpreter/CommandInterpreter.h"
#include "lldb/Interpreter/CommandReturnObject.h"
#include "lldb/lldb-private-log.h"
#include "lldb/Symbol/ObjectFile.h"
#include "lldb/Target/Process.h"
#include "lldb/Target/StackFrame.h"
#include "lldb/Target/Thread.h"
#include "lldb/Target/ThreadSpec.h"

using namespace lldb;
using namespace lldb_private;

//----------------------------------------------------------------------
// Target constructor
//----------------------------------------------------------------------
Target::Target(Debugger &debugger, const lldb::PlatformSP &platform_sp) :
    Broadcaster("lldb.target"),
    m_platform_sp (platform_sp),
    TargetInstanceSettings (*GetSettingsController()),
    m_debugger (debugger),
    m_mutex (Mutex::eMutexTypeRecursive), 
    m_images(),
    m_section_load_list (),
    m_breakpoint_list (false),
    m_internal_breakpoint_list (true),
    m_process_sp(),
    m_search_filter_sp(),
    m_image_search_paths (ImageSearchPathsChanged, this),
    m_scratch_ast_context_ap (NULL),
    m_persistent_variables (),
    m_stop_hook_next_id(0)
{
    SetEventName (eBroadcastBitBreakpointChanged, "breakpoint-changed");
    SetEventName (eBroadcastBitModulesLoaded, "modules-loaded");
    SetEventName (eBroadcastBitModulesUnloaded, "modules-unloaded");

    LogSP log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_OBJECT));
    if (log)
        log->Printf ("%p Target::Target()", this);
}

//----------------------------------------------------------------------
// Destructor
//----------------------------------------------------------------------
Target::~Target()
{
    LogSP log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_OBJECT));
    if (log)
        log->Printf ("%p Target::~Target()", this);
    DeleteCurrentProcess ();
}

void
Target::Dump (Stream *s, lldb::DescriptionLevel description_level)
{
//    s->Printf("%.*p: ", (int)sizeof(void*) * 2, this);
    if (description_level != lldb::eDescriptionLevelBrief)
    {
        s->Indent();
        s->PutCString("Target\n");
        s->IndentMore();
            m_images.Dump(s);
            m_breakpoint_list.Dump(s);
            m_internal_breakpoint_list.Dump(s);
        s->IndentLess();
    }
    else
    {
        s->PutCString (GetExecutableModule()->GetFileSpec().GetFilename().GetCString());
    }
}

void
Target::DeleteCurrentProcess ()
{
    if (m_process_sp.get())
    {
        m_section_load_list.Clear();
        if (m_process_sp->IsAlive())
            m_process_sp->Destroy();
        
        m_process_sp->Finalize();

        // Do any cleanup of the target we need to do between process instances.
        // NB It is better to do this before destroying the process in case the
        // clean up needs some help from the process.
        m_breakpoint_list.ClearAllBreakpointSites();
        m_internal_breakpoint_list.ClearAllBreakpointSites();
        m_process_sp.reset();
    }
}

const lldb::ProcessSP &
Target::CreateProcess (Listener &listener, const char *plugin_name)
{
    DeleteCurrentProcess ();
    m_process_sp.reset(Process::FindPlugin(*this, plugin_name, listener));
    return m_process_sp;
}

const lldb::ProcessSP &
Target::GetProcessSP () const
{
    return m_process_sp;
}

lldb::TargetSP
Target::GetSP()
{
    return m_debugger.GetTargetList().GetTargetSP(this);
}

BreakpointList &
Target::GetBreakpointList(bool internal)
{
    if (internal)
        return m_internal_breakpoint_list;
    else
        return m_breakpoint_list;
}

const BreakpointList &
Target::GetBreakpointList(bool internal) const
{
    if (internal)
        return m_internal_breakpoint_list;
    else
        return m_breakpoint_list;
}

BreakpointSP
Target::GetBreakpointByID (break_id_t break_id)
{
    BreakpointSP bp_sp;

    if (LLDB_BREAK_ID_IS_INTERNAL (break_id))
        bp_sp = m_internal_breakpoint_list.FindBreakpointByID (break_id);
    else
        bp_sp = m_breakpoint_list.FindBreakpointByID (break_id);

    return bp_sp;
}

BreakpointSP
Target::CreateBreakpoint (const FileSpec *containingModule, const FileSpec &file, uint32_t line_no, bool check_inlines, bool internal)
{
    SearchFilterSP filter_sp(GetSearchFilterForModule (containingModule));
    BreakpointResolverSP resolver_sp(new BreakpointResolverFileLine (NULL, file, line_no, check_inlines));
    return CreateBreakpoint (filter_sp, resolver_sp, internal);
}


BreakpointSP
Target::CreateBreakpoint (lldb::addr_t addr, bool internal)
{
    Address so_addr;
    // Attempt to resolve our load address if possible, though it is ok if
    // it doesn't resolve to section/offset.

    // Try and resolve as a load address if possible
    m_section_load_list.ResolveLoadAddress(addr, so_addr);
    if (!so_addr.IsValid())
    {
        // The address didn't resolve, so just set this as an absolute address
        so_addr.SetOffset (addr);
    }
    BreakpointSP bp_sp (CreateBreakpoint(so_addr, internal));
    return bp_sp;
}

BreakpointSP
Target::CreateBreakpoint (Address &addr, bool internal)
{
    TargetSP target_sp = this->GetSP();
    SearchFilterSP filter_sp(new SearchFilter (target_sp));
    BreakpointResolverSP resolver_sp (new BreakpointResolverAddress (NULL, addr));
    return CreateBreakpoint (filter_sp, resolver_sp, internal);
}

BreakpointSP
Target::CreateBreakpoint (FileSpec *containingModule, const char *func_name, uint32_t func_name_type_mask, bool internal)
{
    BreakpointSP bp_sp;
    if (func_name)
    {
        SearchFilterSP filter_sp(GetSearchFilterForModule (containingModule));
        BreakpointResolverSP resolver_sp (new BreakpointResolverName (NULL, func_name, func_name_type_mask, Breakpoint::Exact));
        bp_sp = CreateBreakpoint (filter_sp, resolver_sp, internal);
    }
    return bp_sp;
}


SearchFilterSP
Target::GetSearchFilterForModule (const FileSpec *containingModule)
{
    SearchFilterSP filter_sp;
    lldb::TargetSP target_sp = this->GetSP();
    if (containingModule != NULL)
    {
        // TODO: We should look into sharing module based search filters
        // across many breakpoints like we do for the simple target based one
        filter_sp.reset (new SearchFilterByModule (target_sp, *containingModule));
    }
    else
    {
        if (m_search_filter_sp.get() == NULL)
            m_search_filter_sp.reset (new SearchFilter (target_sp));
        filter_sp = m_search_filter_sp;
    }
    return filter_sp;
}

BreakpointSP
Target::CreateBreakpoint (FileSpec *containingModule, RegularExpression &func_regex, bool internal)
{
    SearchFilterSP filter_sp(GetSearchFilterForModule (containingModule));
    BreakpointResolverSP resolver_sp(new BreakpointResolverName (NULL, func_regex));

    return CreateBreakpoint (filter_sp, resolver_sp, internal);
}

BreakpointSP
Target::CreateBreakpoint (SearchFilterSP &filter_sp, BreakpointResolverSP &resolver_sp, bool internal)
{
    BreakpointSP bp_sp;
    if (filter_sp && resolver_sp)
    {
        bp_sp.reset(new Breakpoint (*this, filter_sp, resolver_sp));
        resolver_sp->SetBreakpoint (bp_sp.get());

        if (internal)
            m_internal_breakpoint_list.Add (bp_sp, false);
        else
            m_breakpoint_list.Add (bp_sp, true);

        LogSP log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_BREAKPOINTS));
        if (log)
        {
            StreamString s;
            bp_sp->GetDescription(&s, lldb::eDescriptionLevelVerbose);
            log->Printf ("Target::%s (internal = %s) => break_id = %s\n", __FUNCTION__, internal ? "yes" : "no", s.GetData());
        }

        bp_sp->ResolveBreakpoint();
    }
    
    if (!internal && bp_sp)
    {
        m_last_created_breakpoint = bp_sp;
    }
    
    return bp_sp;
}

void
Target::RemoveAllBreakpoints (bool internal_also)
{
    LogSP log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_BREAKPOINTS));
    if (log)
        log->Printf ("Target::%s (internal_also = %s)\n", __FUNCTION__, internal_also ? "yes" : "no");

    m_breakpoint_list.RemoveAll (true);
    if (internal_also)
        m_internal_breakpoint_list.RemoveAll (false);
        
    m_last_created_breakpoint.reset();
}

void
Target::DisableAllBreakpoints (bool internal_also)
{
    LogSP log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_BREAKPOINTS));
    if (log)
        log->Printf ("Target::%s (internal_also = %s)\n", __FUNCTION__, internal_also ? "yes" : "no");

    m_breakpoint_list.SetEnabledAll (false);
    if (internal_also)
        m_internal_breakpoint_list.SetEnabledAll (false);
}

void
Target::EnableAllBreakpoints (bool internal_also)
{
    LogSP log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_BREAKPOINTS));
    if (log)
        log->Printf ("Target::%s (internal_also = %s)\n", __FUNCTION__, internal_also ? "yes" : "no");

    m_breakpoint_list.SetEnabledAll (true);
    if (internal_also)
        m_internal_breakpoint_list.SetEnabledAll (true);
}

bool
Target::RemoveBreakpointByID (break_id_t break_id)
{
    LogSP log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_BREAKPOINTS));
    if (log)
        log->Printf ("Target::%s (break_id = %i, internal = %s)\n", __FUNCTION__, break_id, LLDB_BREAK_ID_IS_INTERNAL (break_id) ? "yes" : "no");

    if (DisableBreakpointByID (break_id))
    {
        if (LLDB_BREAK_ID_IS_INTERNAL (break_id))
            m_internal_breakpoint_list.Remove(break_id, false);
        else
        {
            if (m_last_created_breakpoint)
            {
                if (m_last_created_breakpoint->GetID() == break_id)
                    m_last_created_breakpoint.reset();
            }
            m_breakpoint_list.Remove(break_id, true);
        }
        return true;
    }
    return false;
}

bool
Target::DisableBreakpointByID (break_id_t break_id)
{
    LogSP log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_BREAKPOINTS));
    if (log)
        log->Printf ("Target::%s (break_id = %i, internal = %s)\n", __FUNCTION__, break_id, LLDB_BREAK_ID_IS_INTERNAL (break_id) ? "yes" : "no");

    BreakpointSP bp_sp;

    if (LLDB_BREAK_ID_IS_INTERNAL (break_id))
        bp_sp = m_internal_breakpoint_list.FindBreakpointByID (break_id);
    else
        bp_sp = m_breakpoint_list.FindBreakpointByID (break_id);
    if (bp_sp)
    {
        bp_sp->SetEnabled (false);
        return true;
    }
    return false;
}

bool
Target::EnableBreakpointByID (break_id_t break_id)
{
    LogSP log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_BREAKPOINTS));
    if (log)
        log->Printf ("Target::%s (break_id = %i, internal = %s)\n",
                     __FUNCTION__,
                     break_id,
                     LLDB_BREAK_ID_IS_INTERNAL (break_id) ? "yes" : "no");

    BreakpointSP bp_sp;

    if (LLDB_BREAK_ID_IS_INTERNAL (break_id))
        bp_sp = m_internal_breakpoint_list.FindBreakpointByID (break_id);
    else
        bp_sp = m_breakpoint_list.FindBreakpointByID (break_id);

    if (bp_sp)
    {
        bp_sp->SetEnabled (true);
        return true;
    }
    return false;
}

ModuleSP
Target::GetExecutableModule ()
{
    ModuleSP executable_sp;
    if (m_images.GetSize() > 0)
        executable_sp = m_images.GetModuleAtIndex(0);
    return executable_sp;
}

void
Target::SetExecutableModule (ModuleSP& executable_sp, bool get_dependent_files)
{
    m_images.Clear();
    m_scratch_ast_context_ap.reset();
    
    if (executable_sp.get())
    {
        Timer scoped_timer (__PRETTY_FUNCTION__,
                            "Target::SetExecutableModule (executable = '%s/%s')",
                            executable_sp->GetFileSpec().GetDirectory().AsCString(),
                            executable_sp->GetFileSpec().GetFilename().AsCString());

        m_images.Append(executable_sp); // The first image is our exectuable file

        ArchSpec exe_arch = executable_sp->GetArchitecture();
        // If we haven't set an architecture yet, reset our architecture based on what we found in the executable module.
        if (!m_arch_spec.IsValid())
            m_arch_spec = exe_arch;
        
        FileSpecList dependent_files;
        ObjectFile *executable_objfile = executable_sp->GetObjectFile();

        if (executable_objfile)
        {
            executable_objfile->GetDependentModules(dependent_files);
            for (uint32_t i=0; i<dependent_files.GetSize(); i++)
            {
                FileSpec dependent_file_spec (dependent_files.GetFileSpecPointerAtIndex(i));
                FileSpec platform_dependent_file_spec;
                if (m_platform_sp)
                    m_platform_sp->GetFile (dependent_file_spec, NULL, platform_dependent_file_spec);
                else
                    platform_dependent_file_spec = dependent_file_spec;

                ModuleSP image_module_sp(GetSharedModule (platform_dependent_file_spec,
                                                          exe_arch));
                if (image_module_sp.get())
                {
                    //image_module_sp->Dump(&s);// REMOVE THIS, DEBUG ONLY
                    ObjectFile *objfile = image_module_sp->GetObjectFile();
                    if (objfile)
                        objfile->GetDependentModules(dependent_files);
                }
            }
        }
        
        // Now see if we know the target triple, and if so, create our scratch AST context:
        if (m_arch_spec.IsValid())
        {
            m_scratch_ast_context_ap.reset (new ClangASTContext(m_arch_spec.GetTriple().str().c_str()));
        }
    }

    UpdateInstanceName();
}


bool
Target::SetArchitecture (const ArchSpec &arch_spec)
{
    if (m_arch_spec == arch_spec)
    {
        // If we're setting the architecture to our current architecture, we
        // don't need to do anything.
        return true;
    }
    else if (!m_arch_spec.IsValid())
    {
        // If we haven't got a valid arch spec, then we just need to set it.
        m_arch_spec = arch_spec;
        return true;
    }
    else
    {
        // If we have an executable file, try to reset the executable to the desired architecture
        m_arch_spec = arch_spec;
        ModuleSP executable_sp = GetExecutableModule ();
        m_images.Clear();
        m_scratch_ast_context_ap.reset();
        // Need to do something about unsetting breakpoints.
        
        if (executable_sp)
        {
            FileSpec exec_file_spec = executable_sp->GetFileSpec();
            Error error = ModuleList::GetSharedModule(exec_file_spec, 
                                                      arch_spec, 
                                                      NULL, 
                                                      NULL, 
                                                      0, 
                                                      executable_sp, 
                                                      NULL, 
                                                      NULL);
                                          
            if (!error.Fail() && executable_sp)
            {
                SetExecutableModule (executable_sp, true);
                return true;
            }
            else
            {
                return false;
            }
        }
        else
        {
            return false;
        }
    }
}

void
Target::ModuleAdded (ModuleSP &module_sp)
{
    // A module is being added to this target for the first time
    ModuleList module_list;
    module_list.Append(module_sp);
    ModulesDidLoad (module_list);
}

void
Target::ModuleUpdated (ModuleSP &old_module_sp, ModuleSP &new_module_sp)
{
    // A module is being added to this target for the first time
    ModuleList module_list;
    module_list.Append (old_module_sp);
    ModulesDidUnload (module_list);
    module_list.Clear ();
    module_list.Append (new_module_sp);
    ModulesDidLoad (module_list);
}

void
Target::ModulesDidLoad (ModuleList &module_list)
{
    m_breakpoint_list.UpdateBreakpoints (module_list, true);
    // TODO: make event data that packages up the module_list
    BroadcastEvent (eBroadcastBitModulesLoaded, NULL);
}

void
Target::ModulesDidUnload (ModuleList &module_list)
{
    m_breakpoint_list.UpdateBreakpoints (module_list, false);

    // Remove the images from the target image list
    m_images.Remove(module_list);

    // TODO: make event data that packages up the module_list
    BroadcastEvent (eBroadcastBitModulesUnloaded, NULL);
}

size_t
Target::ReadMemoryFromFileCache (const Address& addr, void *dst, size_t dst_len, Error &error)
{
    const Section *section = addr.GetSection();
    if (section && section->GetModule())
    {
        ObjectFile *objfile = section->GetModule()->GetObjectFile();
        if (objfile)
        {
            size_t bytes_read = section->ReadSectionDataFromObjectFile (objfile, 
                                                                        addr.GetOffset(), 
                                                                        dst, 
                                                                        dst_len);
            if (bytes_read > 0)
                return bytes_read;
            else
                error.SetErrorStringWithFormat("error reading data from section %s", section->GetName().GetCString());
        }
        else
        {
            error.SetErrorString("address isn't from a object file");
        }
    }
    else
    {
        error.SetErrorString("address doesn't contain a section that points to a section in a object file");
    }
    return 0;
}

size_t
Target::ReadMemory (const Address& addr, bool prefer_file_cache, void *dst, size_t dst_len, Error &error)
{
    error.Clear();
    
    bool process_is_valid = m_process_sp && m_process_sp->IsAlive();

    size_t bytes_read = 0;
    Address resolved_addr(addr);
    if (!resolved_addr.IsSectionOffset())
    {
        if (process_is_valid)
        {
            m_section_load_list.ResolveLoadAddress (addr.GetOffset(), resolved_addr);
        }
        else
        {
            m_images.ResolveFileAddress(addr.GetOffset(), resolved_addr);
        }
    }
    
    if (prefer_file_cache)
    {
        bytes_read = ReadMemoryFromFileCache (resolved_addr, dst, dst_len, error);
        if (bytes_read > 0)
            return bytes_read;
    }
    
    if (process_is_valid)
    {
        lldb::addr_t load_addr = resolved_addr.GetLoadAddress (this);
        if (load_addr == LLDB_INVALID_ADDRESS)
        {
            if (resolved_addr.GetModule() && resolved_addr.GetModule()->GetFileSpec())
                error.SetErrorStringWithFormat("%s[0x%llx] can't be resolved, %s in not currently loaded.\n", 
                                               resolved_addr.GetModule()->GetFileSpec().GetFilename().AsCString(), 
                                               resolved_addr.GetFileAddress());
            else
                error.SetErrorStringWithFormat("0x%llx can't be resolved.\n", resolved_addr.GetFileAddress());
        }
        else
        {
            bytes_read = m_process_sp->ReadMemory(load_addr, dst, dst_len, error);
            if (bytes_read != dst_len)
            {
                if (error.Success())
                {
                    if (bytes_read == 0)
                        error.SetErrorStringWithFormat("Read memory from 0x%llx failed.\n", load_addr);
                    else
                        error.SetErrorStringWithFormat("Only %zu of %zu bytes were read from memory at 0x%llx.\n", bytes_read, dst_len, load_addr);
                }
            }
            if (bytes_read)
                return bytes_read;
            // If the address is not section offset we have an address that
            // doesn't resolve to any address in any currently loaded shared
            // libaries and we failed to read memory so there isn't anything
            // more we can do. If it is section offset, we might be able to
            // read cached memory from the object file.
            if (!resolved_addr.IsSectionOffset())
                return 0;
        }
    }
    
    if (!prefer_file_cache)
    {
        // If we didn't already try and read from the object file cache, then
        // try it after failing to read from the process.
        return ReadMemoryFromFileCache (resolved_addr, dst, dst_len, error);
    }
    return 0;
}


ModuleSP
Target::GetSharedModule
(
    const FileSpec& file_spec,
    const ArchSpec& arch,
    const lldb_private::UUID *uuid_ptr,
    const ConstString *object_name,
    off_t object_offset,
    Error *error_ptr
)
{
    // Don't pass in the UUID so we can tell if we have a stale value in our list
    ModuleSP old_module_sp; // This will get filled in if we have a new version of the library
    bool did_create_module = false;
    ModuleSP module_sp;

    // If there are image search path entries, try to use them first to acquire a suitable image.

    Error error;

    if (m_image_search_paths.GetSize())
    {
        FileSpec transformed_spec;        
        if (m_image_search_paths.RemapPath (file_spec.GetDirectory(), transformed_spec.GetDirectory()))
        {
            transformed_spec.GetFilename() = file_spec.GetFilename();
            error = ModuleList::GetSharedModule (transformed_spec, arch, uuid_ptr, object_name, object_offset, module_sp, &old_module_sp, &did_create_module);
        }
    }

    // If a module hasn't been found yet, use the unmodified path.

    if (!module_sp)
    {
        error = (ModuleList::GetSharedModule (file_spec, arch, uuid_ptr, object_name, object_offset, module_sp, &old_module_sp, &did_create_module));
    }

    if (module_sp)
    {
        m_images.Append (module_sp);
        if (did_create_module)
        {
            if (old_module_sp && m_images.GetIndexForModule (old_module_sp.get()) != LLDB_INVALID_INDEX32)
                ModuleUpdated(old_module_sp, module_sp);
            else
                ModuleAdded(module_sp);
        }
    }
    if (error_ptr)
        *error_ptr = error;
    return module_sp;
}


Target *
Target::CalculateTarget ()
{
    return this;
}

Process *
Target::CalculateProcess ()
{
    return NULL;
}

Thread *
Target::CalculateThread ()
{
    return NULL;
}

StackFrame *
Target::CalculateStackFrame ()
{
    return NULL;
}

void
Target::CalculateExecutionContext (ExecutionContext &exe_ctx)
{
    exe_ctx.target = this;
    exe_ctx.process = NULL; // Do NOT fill in process...
    exe_ctx.thread = NULL;
    exe_ctx.frame = NULL;
}

PathMappingList &
Target::GetImageSearchPathList ()
{
    return m_image_search_paths;
}

void
Target::ImageSearchPathsChanged 
(
    const PathMappingList &path_list,
    void *baton
)
{
    Target *target = (Target *)baton;
    if (target->m_images.GetSize() > 1)
    {
        ModuleSP exe_module_sp (target->GetExecutableModule());
        if (exe_module_sp)
        {
            target->m_images.Clear();
            target->SetExecutableModule (exe_module_sp, true);
        }
    }
}

ClangASTContext *
Target::GetScratchClangASTContext()
{
    return m_scratch_ast_context_ap.get();
}

void
Target::SettingsInitialize ()
{
    UserSettingsControllerSP &usc = GetSettingsController();
    usc.reset (new SettingsController);
    UserSettingsController::InitializeSettingsController (usc,
                                                          SettingsController::global_settings_table,
                                                          SettingsController::instance_settings_table);
                                                          
    // Now call SettingsInitialize() on each 'child' setting of Target
    Process::SettingsInitialize ();
}

void
Target::SettingsTerminate ()
{

    // Must call SettingsTerminate() on each settings 'child' of Target, before terminating Target's Settings.
    
    Process::SettingsTerminate ();
    
    // Now terminate Target Settings.
    
    UserSettingsControllerSP &usc = GetSettingsController();
    UserSettingsController::FinalizeSettingsController (usc);
    usc.reset();
}

UserSettingsControllerSP &
Target::GetSettingsController ()
{
    static UserSettingsControllerSP g_settings_controller;
    return g_settings_controller;
}

ArchSpec
Target::GetDefaultArchitecture ()
{
    lldb::UserSettingsControllerSP settings_controller_sp (GetSettingsController());
    
    if (settings_controller_sp)
        return static_cast<Target::SettingsController *>(settings_controller_sp.get())->GetArchitecture ();
    return ArchSpec();
}

void
Target::SetDefaultArchitecture (const ArchSpec& arch)
{
    lldb::UserSettingsControllerSP settings_controller_sp (GetSettingsController());

    if (settings_controller_sp)
        static_cast<Target::SettingsController *>(settings_controller_sp.get())->GetArchitecture () = arch;
}

Target *
Target::GetTargetFromContexts (const ExecutionContext *exe_ctx_ptr, const SymbolContext *sc_ptr)
{
    // The target can either exist in the "process" of ExecutionContext, or in 
    // the "target_sp" member of SymbolContext. This accessor helper function
    // will get the target from one of these locations.

    Target *target = NULL;
    if (sc_ptr != NULL)
        target = sc_ptr->target_sp.get();
    if (target == NULL)
    {
        if (exe_ctx_ptr != NULL && exe_ctx_ptr->process != NULL)
            target = &exe_ctx_ptr->process->GetTarget();
    }
    return target;
}


void
Target::UpdateInstanceName ()
{
    StreamString sstr;
    
    ModuleSP module_sp = GetExecutableModule();
    if (module_sp)
    {
        sstr.Printf ("%s_%s", 
                     module_sp->GetFileSpec().GetFilename().AsCString(), 
                     module_sp->GetArchitecture().GetArchitectureName());
        GetSettingsController()->RenameInstanceSettings (GetInstanceName().AsCString(),
                                                         sstr.GetData());
    }
}

const char *
Target::GetExpressionPrefixContentsAsCString ()
{
    return m_expr_prefix_contents.c_str();
}

ExecutionResults
Target::EvaluateExpression
(
    const char *expr_cstr,
    StackFrame *frame,
    bool unwind_on_error,
    bool keep_in_memory,
    lldb::ValueObjectSP &result_valobj_sp
)
{
    ExecutionResults execution_results = eExecutionSetupError;

    result_valobj_sp.reset();

    ExecutionContext exe_ctx;
    if (frame)
    {
        frame->CalculateExecutionContext(exe_ctx);
        Error error;
        const uint32_t expr_path_options = StackFrame::eExpressionPathOptionCheckPtrVsMember |
                                           StackFrame::eExpressionPathOptionsNoFragileObjcIvar;
        result_valobj_sp = frame->GetValueForVariableExpressionPath (expr_cstr, expr_path_options, error);
    }
    else if (m_process_sp)
    {
        m_process_sp->CalculateExecutionContext(exe_ctx);
    }
    else
    {
        CalculateExecutionContext(exe_ctx);
    }
    
    if (result_valobj_sp)
    {
        execution_results = eExecutionCompleted;
        // We got a result from the frame variable expression path above...
        ConstString persistent_variable_name (m_persistent_variables.GetNextPersistentVariableName());

        lldb::ValueObjectSP const_valobj_sp;
        
        // Check in case our value is already a constant value
        if (result_valobj_sp->GetIsConstant())
        {
            const_valobj_sp = result_valobj_sp;
            const_valobj_sp->SetName (persistent_variable_name);
        }
        else
            const_valobj_sp = result_valobj_sp->CreateConstantValue (exe_ctx.GetBestExecutionContextScope(), 
                                                                     persistent_variable_name);

        lldb::ValueObjectSP live_valobj_sp = result_valobj_sp;
        
        result_valobj_sp = const_valobj_sp;

        ClangExpressionVariableSP clang_expr_variable_sp(m_persistent_variables.CreatePersistentVariable(result_valobj_sp));        
        assert (clang_expr_variable_sp.get());
        
        // Set flags and live data as appropriate

        const Value &result_value = live_valobj_sp->GetValue();
        
        switch (result_value.GetValueType())
        {
        case Value::eValueTypeHostAddress:
        case Value::eValueTypeFileAddress:
            // we don't do anything with these for now
            break;
        case Value::eValueTypeScalar:
            clang_expr_variable_sp->m_flags |= ClangExpressionVariable::EVIsLLDBAllocated;
            clang_expr_variable_sp->m_flags |= ClangExpressionVariable::EVNeedsAllocation;
            break;
        case Value::eValueTypeLoadAddress:
            clang_expr_variable_sp->m_live_sp = live_valobj_sp;
            clang_expr_variable_sp->m_flags |= ClangExpressionVariable::EVIsProgramReference;
            break;
        }
    }
    else
    {
        // Make sure we aren't just trying to see the value of a persistent 
        // variable (something like "$0")
        lldb::ClangExpressionVariableSP persistent_var_sp;
        // Only check for persistent variables the expression starts with a '$' 
        if (expr_cstr[0] == '$')
            persistent_var_sp = m_persistent_variables.GetVariable (expr_cstr);

        if (persistent_var_sp)
        {
            result_valobj_sp = persistent_var_sp->GetValueObject ();
            execution_results = eExecutionCompleted;
        }
        else
        {
            const char *prefix = GetExpressionPrefixContentsAsCString();
        
            execution_results = ClangUserExpression::Evaluate (exe_ctx, 
                                                               unwind_on_error,
                                                               keep_in_memory,
                                                               expr_cstr, 
                                                               prefix, 
                                                               result_valobj_sp);
        }
    }
    return execution_results;
}

lldb::user_id_t
Target::AddStopHook (Target::StopHookSP &new_hook_sp)
{
    lldb::user_id_t new_uid = ++m_stop_hook_next_id;
    new_hook_sp.reset (new StopHook(GetSP(), new_uid));
    m_stop_hooks[new_uid] = new_hook_sp;
    return new_uid;
}

bool
Target::RemoveStopHookByID (lldb::user_id_t user_id)
{
    size_t num_removed;
    num_removed = m_stop_hooks.erase (user_id);
    if (num_removed == 0)
        return false;
    else
        return true;
}

void
Target::RemoveAllStopHooks ()
{
    m_stop_hooks.clear();
}

Target::StopHookSP
Target::GetStopHookByID (lldb::user_id_t user_id)
{
    StopHookSP found_hook;
    
    StopHookCollection::iterator specified_hook_iter;
    specified_hook_iter = m_stop_hooks.find (user_id);
    if (specified_hook_iter != m_stop_hooks.end())
        found_hook = (*specified_hook_iter).second;
    return found_hook;
}

bool
Target::SetStopHookActiveStateByID (lldb::user_id_t user_id, bool active_state)
{
    StopHookCollection::iterator specified_hook_iter;
    specified_hook_iter = m_stop_hooks.find (user_id);
    if (specified_hook_iter == m_stop_hooks.end())
        return false;
        
    (*specified_hook_iter).second->SetIsActive (active_state);
    return true;
}

void
Target::SetAllStopHooksActiveState (bool active_state)
{
    StopHookCollection::iterator pos, end = m_stop_hooks.end();
    for (pos = m_stop_hooks.begin(); pos != end; pos++)
    {
        (*pos).second->SetIsActive (active_state);
    }
}

void
Target::RunStopHooks ()
{
    if (!m_process_sp)
        return;
        
    if (m_stop_hooks.empty())
        return;
        
    StopHookCollection::iterator pos, end = m_stop_hooks.end();
        
    // If there aren't any active stop hooks, don't bother either:
    bool any_active_hooks = false;
    for (pos = m_stop_hooks.begin(); pos != end; pos++)
    {
        if ((*pos).second->IsActive())
        {
            any_active_hooks = true;
            break;
        }
    }
    if (!any_active_hooks)
        return;
    
    CommandReturnObject result;
    
    std::vector<ExecutionContext> exc_ctx_with_reasons;
    std::vector<SymbolContext> sym_ctx_with_reasons;
    
    ThreadList &cur_threadlist = m_process_sp->GetThreadList();
    size_t num_threads = cur_threadlist.GetSize();
    for (size_t i = 0; i < num_threads; i++)
    {
        lldb::ThreadSP cur_thread_sp = cur_threadlist.GetThreadAtIndex (i);
        if (cur_thread_sp->ThreadStoppedForAReason())
        {
            lldb::StackFrameSP cur_frame_sp = cur_thread_sp->GetStackFrameAtIndex(0);
            exc_ctx_with_reasons.push_back(ExecutionContext(m_process_sp.get(), cur_thread_sp.get(), cur_frame_sp.get()));
            sym_ctx_with_reasons.push_back(cur_frame_sp->GetSymbolContext(eSymbolContextEverything));
        }
    }
    
    // If no threads stopped for a reason, don't run the stop-hooks.
    size_t num_exe_ctx = exc_ctx_with_reasons.size();
    if (num_exe_ctx == 0)
        return;
    
    result.SetImmediateOutputFile (m_debugger.GetOutputFile().GetStream());
    result.SetImmediateErrorFile (m_debugger.GetErrorFile().GetStream());
    
    bool keep_going = true;
    bool hooks_ran = false;
    bool print_hook_header;
    bool print_thread_header;
    
    if (num_exe_ctx == 1)
        print_thread_header = false;
    else
        print_thread_header = true;
        
    if (m_stop_hooks.size() == 1)
        print_hook_header = false;
    else
        print_hook_header = true;
        
    for (pos = m_stop_hooks.begin(); keep_going && pos != end; pos++)
    {
        // result.Clear();
        StopHookSP cur_hook_sp = (*pos).second;
        if (!cur_hook_sp->IsActive())
            continue;
        
        bool any_thread_matched = false;
        for (size_t i = 0; keep_going && i < num_exe_ctx; i++)
        {
            if ((cur_hook_sp->GetSpecifier () == NULL 
                  || cur_hook_sp->GetSpecifier()->SymbolContextMatches(sym_ctx_with_reasons[i]))
                && (cur_hook_sp->GetThreadSpecifier() == NULL
                    || cur_hook_sp->GetThreadSpecifier()->ThreadPassesBasicTests(exc_ctx_with_reasons[i].thread)))
            {
                if (!hooks_ran)
                {
                    result.AppendMessage("\n** Stop Hooks **");
                    hooks_ran = true;
                }
                if (print_hook_header && !any_thread_matched)
                {
                    result.AppendMessageWithFormat("\n- Hook %d\n", cur_hook_sp->GetID());
                    any_thread_matched = true;
                }
                
                if (print_thread_header)
                    result.AppendMessageWithFormat("-- Thread %d\n", exc_ctx_with_reasons[i].thread->GetIndexID());
                
                bool stop_on_continue = true; 
                bool stop_on_error = true; 
                bool echo_commands = false;
                bool print_results = true; 
                GetDebugger().GetCommandInterpreter().HandleCommands (cur_hook_sp->GetCommands(), 
                                                                          &exc_ctx_with_reasons[i], 
                                                                          stop_on_continue, 
                                                                          stop_on_error, 
                                                                          echo_commands,
                                                                          print_results, 
                                                                          result);

                // If the command started the target going again, we should bag out of
                // running the stop hooks.
                if ((result.GetStatus() == eReturnStatusSuccessContinuingNoResult)
                        || (result.GetStatus() == eReturnStatusSuccessContinuingResult))
                {
                    result.AppendMessageWithFormat ("Aborting stop hooks, hook %d set the program running.", cur_hook_sp->GetID());
                    keep_going = false;
                }
            }
        }
    }
    if (hooks_ran)
        result.AppendMessage ("\n** End Stop Hooks **\n");
}

//--------------------------------------------------------------
// class Target::StopHook
//--------------------------------------------------------------


Target::StopHook::StopHook (lldb::TargetSP target_sp, lldb::user_id_t uid) :
        UserID (uid),
        m_target_sp (target_sp),
        m_active (true),
        m_commands (),
        m_specifier_sp (),
        m_thread_spec_ap(NULL)
{
}

Target::StopHook::StopHook (const StopHook &rhs) :
        UserID (rhs.GetID()),
        m_target_sp (rhs.m_target_sp),
        m_commands (rhs.m_commands),
        m_specifier_sp (rhs.m_specifier_sp),
        m_active (rhs.m_active),
        m_thread_spec_ap (NULL)
{
    if (rhs.m_thread_spec_ap.get() != NULL)
        m_thread_spec_ap.reset (new ThreadSpec(*rhs.m_thread_spec_ap.get()));
}
        

Target::StopHook::~StopHook ()
{
}

void
Target::StopHook::SetThreadSpecifier (ThreadSpec *specifier)
{
    m_thread_spec_ap.reset (specifier);
}
        

void
Target::StopHook::GetDescription (Stream *s, lldb::DescriptionLevel level) const
{
    int indent_level = s->GetIndentLevel();

    s->SetIndentLevel(indent_level + 2);

    s->Printf ("Hook: %d\n", GetID());
    if (m_active)
        s->Indent ("State: enabled\n");
    else
        s->Indent ("State: disabled\n");    
    
    if (m_specifier_sp)
    {
        s->Indent();
        s->PutCString ("Specifier:\n");
        s->SetIndentLevel (indent_level + 4);
        m_specifier_sp->GetDescription (s, level);
        s->SetIndentLevel (indent_level + 2);
    }

    if (m_thread_spec_ap.get() != NULL)
    {
        StreamString tmp;
        s->Indent("Thread:\n");
        m_thread_spec_ap->GetDescription (&tmp, level);
        s->SetIndentLevel (indent_level + 4);
        s->Indent (tmp.GetData());
        s->PutCString ("\n");
        s->SetIndentLevel (indent_level + 2);
    }

    s->Indent ("Commands: \n");
    s->SetIndentLevel (indent_level + 4);
    uint32_t num_commands = m_commands.GetSize();
    for (uint32_t i = 0; i < num_commands; i++)
    {
        s->Indent(m_commands.GetStringAtIndex(i));
        s->PutCString ("\n");
    }
    s->SetIndentLevel (indent_level);
}


//--------------------------------------------------------------
// class Target::SettingsController
//--------------------------------------------------------------

Target::SettingsController::SettingsController () :
    UserSettingsController ("target", Debugger::GetSettingsController()),
    m_default_architecture ()
{
    m_default_settings.reset (new TargetInstanceSettings (*this, false,
                                                          InstanceSettings::GetDefaultName().AsCString()));
}

Target::SettingsController::~SettingsController ()
{
}

lldb::InstanceSettingsSP
Target::SettingsController::CreateInstanceSettings (const char *instance_name)
{
    TargetInstanceSettings *new_settings = new TargetInstanceSettings (*GetSettingsController(),
                                                                       false, 
                                                                       instance_name);
    lldb::InstanceSettingsSP new_settings_sp (new_settings);
    return new_settings_sp;
}


#define TSC_DEFAULT_ARCH    "default-arch"
#define TSC_EXPR_PREFIX     "expr-prefix"
#define TSC_EXEC_LEVEL      "execution-level"
#define TSC_EXEC_MODE       "execution-mode"
#define TSC_EXEC_OS_TYPE    "execution-os-type"


static const ConstString &
GetSettingNameForDefaultArch ()
{
    static ConstString g_const_string (TSC_DEFAULT_ARCH);

    return g_const_string;
}

static const ConstString &
GetSettingNameForExpressionPrefix ()
{
    static ConstString g_const_string (TSC_EXPR_PREFIX);
    return g_const_string;
}

static const ConstString &
GetSettingNameForExecutionLevel ()
{
    static ConstString g_const_string (TSC_EXEC_LEVEL);
    return g_const_string;
}

static const ConstString &
GetSettingNameForExecutionMode ()
{
    static ConstString g_const_string (TSC_EXEC_MODE);
    return g_const_string;
}

static const ConstString &
GetSettingNameForExecutionOSType ()
{
    static ConstString g_const_string (TSC_EXEC_OS_TYPE);
    return g_const_string;
}


bool
Target::SettingsController::SetGlobalVariable (const ConstString &var_name,
                                               const char *index_value,
                                               const char *value,
                                               const SettingEntry &entry,
                                               const VarSetOperationType op,
                                               Error&err)
{
    if (var_name == GetSettingNameForDefaultArch())
    {
        m_default_architecture.SetTriple (value);
        if (!m_default_architecture.IsValid())
            err.SetErrorStringWithFormat ("'%s' is not a valid architecture or triple.", value);
    }
    return true;
}


bool
Target::SettingsController::GetGlobalVariable (const ConstString &var_name,
                                               StringList &value,
                                               Error &err)
{
    if (var_name == GetSettingNameForDefaultArch())
    {
        // If the arch is invalid (the default), don't show a string for it
        if (m_default_architecture.IsValid())
            value.AppendString (m_default_architecture.GetArchitectureName());
        return true;
    }
    else
        err.SetErrorStringWithFormat ("unrecognized variable name '%s'", var_name.AsCString());

    return false;
}

//--------------------------------------------------------------
// class TargetInstanceSettings
//--------------------------------------------------------------

TargetInstanceSettings::TargetInstanceSettings
(
    UserSettingsController &owner, 
    bool live_instance, 
    const char *name
) :
    InstanceSettings (owner, name ? name : InstanceSettings::InvalidName().AsCString(), live_instance),
    m_expr_prefix_path (),
    m_expr_prefix_contents ()
{
    // CopyInstanceSettings is a pure virtual function in InstanceSettings; it therefore cannot be called
    // until the vtables for TargetInstanceSettings are properly set up, i.e. AFTER all the initializers.
    // For this reason it has to be called here, rather than in the initializer or in the parent constructor.
    // This is true for CreateInstanceName() too.

    if (GetInstanceName () == InstanceSettings::InvalidName())
    {
        ChangeInstanceName (std::string (CreateInstanceName().AsCString()));
        m_owner.RegisterInstanceSettings (this);
    }

    if (live_instance)
    {
        const lldb::InstanceSettingsSP &pending_settings = m_owner.FindPendingSettings (m_instance_name);
        CopyInstanceSettings (pending_settings,false);
        //m_owner.RemovePendingSettings (m_instance_name);
    }
}

TargetInstanceSettings::TargetInstanceSettings (const TargetInstanceSettings &rhs) :
    InstanceSettings (*Target::GetSettingsController(), CreateInstanceName().AsCString())
{
    if (m_instance_name != InstanceSettings::GetDefaultName())
    {
        const lldb::InstanceSettingsSP &pending_settings = m_owner.FindPendingSettings (m_instance_name);
        CopyInstanceSettings (pending_settings,false);
        //m_owner.RemovePendingSettings (m_instance_name);
    }
}

TargetInstanceSettings::~TargetInstanceSettings ()
{
}

TargetInstanceSettings&
TargetInstanceSettings::operator= (const TargetInstanceSettings &rhs)
{
    if (this != &rhs)
    {
    }

    return *this;
}

void
TargetInstanceSettings::UpdateInstanceSettingsVariable (const ConstString &var_name,
                                                        const char *index_value,
                                                        const char *value,
                                                        const ConstString &instance_name,
                                                        const SettingEntry &entry,
                                                        VarSetOperationType op,
                                                        Error &err,
                                                        bool pending)
{
    if (var_name == GetSettingNameForExpressionPrefix ())
    {
        switch (op)
        {
        default:
            err.SetErrorToGenericError ();
            err.SetErrorString ("Unrecognized operation. Cannot update value.\n");
            return;
        case eVarSetOperationAssign:
            {
                FileSpec file_spec(value, true);
                
                if (!file_spec.Exists())
                {
                    err.SetErrorToGenericError ();
                    err.SetErrorStringWithFormat ("%s does not exist.\n", value);
                    return;
                }
                
                DataBufferSP data_sp (file_spec.ReadFileContents());
                
                if (!data_sp && data_sp->GetByteSize() == 0)
                {
                    err.SetErrorToGenericError ();
                    err.SetErrorStringWithFormat ("Couldn't read from %s\n", value);
                    return;
                }
                
                m_expr_prefix_path = value;
                m_expr_prefix_contents.assign(reinterpret_cast<const char *>(data_sp->GetBytes()), data_sp->GetByteSize());
            }
            return;
        case eVarSetOperationAppend:
            err.SetErrorToGenericError ();
            err.SetErrorString ("Cannot append to a path.\n");
            return;
        case eVarSetOperationClear:
            m_expr_prefix_path.clear ();
            m_expr_prefix_contents.clear ();
            return;
        }
    }
}

void
TargetInstanceSettings::CopyInstanceSettings (const lldb::InstanceSettingsSP &new_settings, bool pending)
{
    TargetInstanceSettings *new_settings_ptr = static_cast <TargetInstanceSettings *> (new_settings.get());
    
    if (!new_settings_ptr)
        return;
    
    m_expr_prefix_path      = new_settings_ptr->m_expr_prefix_path;
    m_expr_prefix_contents  = new_settings_ptr->m_expr_prefix_contents;
}

bool
TargetInstanceSettings::GetInstanceSettingsValue (const SettingEntry &entry,
                                                  const ConstString &var_name,
                                                  StringList &value,
                                                  Error *err)
{
    if (var_name == GetSettingNameForExpressionPrefix ())
    {
        value.AppendString (m_expr_prefix_path.c_str(), m_expr_prefix_path.size());
    }
    else 
    {
        if (err)
            err->SetErrorStringWithFormat ("unrecognized variable name '%s'", var_name.AsCString());
        return false;
    }

    return true;
}

const ConstString
TargetInstanceSettings::CreateInstanceName ()
{
    StreamString sstr;
    static int instance_count = 1;
    
    sstr.Printf ("target_%d", instance_count);
    ++instance_count;

    const ConstString ret_val (sstr.GetData());
    return ret_val;
}

//--------------------------------------------------
// Target::SettingsController Variable Tables
//--------------------------------------------------

SettingEntry
Target::SettingsController::global_settings_table[] =
{
    // var-name           var-type           default      enum  init'd hidden help-text
    // =================  ================== ===========  ====  ====== ====== =========================================================================
    { TSC_DEFAULT_ARCH  , eSetVarTypeString , NULL      , NULL, false, false, "Default architecture to choose, when there's a choice." },
    { NULL              , eSetVarTypeNone   , NULL      , NULL, false, false, NULL }
};

SettingEntry
Target::SettingsController::instance_settings_table[] =
{
    // var-name           var-type           default      enum  init'd hidden help-text
    // =================  ================== ===========  ====  ====== ====== =========================================================================
    { TSC_EXPR_PREFIX   , eSetVarTypeString , NULL      , NULL, false, false, "Path to a file containing expressions to be prepended to all expressions." },
    { NULL              , eSetVarTypeNone   , NULL      , NULL, false, false, NULL }
};
