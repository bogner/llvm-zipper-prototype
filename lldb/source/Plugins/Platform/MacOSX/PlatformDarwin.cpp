//===-- PlatformDarwin.cpp --------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "PlatformDarwin.h"

// C Includes
// C++ Includes
// Other libraries and framework includes
// Project includes
#include "lldb/Breakpoint/BreakpointLocation.h"
#include "lldb/Core/Error.h"
#include "lldb/Host/Host.h"
#include "lldb/Target/Target.h"

using namespace lldb;
using namespace lldb_private;
    

//------------------------------------------------------------------
/// Default Constructor
//------------------------------------------------------------------
PlatformDarwin::PlatformDarwin (bool is_host) :
    Platform(is_host),  // This is the local host platform
    m_remote_platform_sp ()
{
}

//------------------------------------------------------------------
/// Destructor.
///
/// The destructor is virtual since this class is designed to be
/// inherited from by the plug-in instance.
//------------------------------------------------------------------
PlatformDarwin::~PlatformDarwin()
{
}


Error
PlatformDarwin::ResolveExecutable (const FileSpec &exe_file,
                                   const ArchSpec &exe_arch,
                                   lldb::ModuleSP &exe_module_sp)
{
    Error error;
    // Nothing special to do here, just use the actual file and architecture

    FileSpec resolved_exe_file (exe_file);
    
    // If we have "ls" as the exe_file, resolve the executable loation based on
    // the current path variables
    if (!resolved_exe_file.Exists())
        resolved_exe_file.ResolveExecutableLocation ();

    // Resolve any executable within a bundle on MacOSX
    Host::ResolveExecutableInBundle (resolved_exe_file);

    if (resolved_exe_file.Exists())
    {
        if (exe_arch.IsValid())
        {
            error = ModuleList::GetSharedModule (resolved_exe_file, 
                                                 exe_arch, 
                                                 NULL,
                                                 NULL, 
                                                 0, 
                                                 exe_module_sp, 
                                                 NULL, 
                                                 NULL);
        
            if (exe_module_sp->GetObjectFile() == NULL)
            {
                exe_module_sp.reset();
                error.SetErrorStringWithFormat ("'%s%s%s' doesn't contain the architecture %s",
                                                exe_file.GetDirectory().AsCString(""),
                                                exe_file.GetDirectory() ? "/" : "",
                                                exe_file.GetFilename().AsCString(""),
                                                exe_arch.GetArchitectureName());
            }
        }
        else
        {
            // No valid architecture was specified, ask the platform for
            // the architectures that we should be using (in the correct order)
            // and see if we can find a match that way
            StreamString arch_names;
            ArchSpec platform_arch;
            for (uint32_t idx = 0; GetSupportedArchitectureAtIndex (idx, platform_arch); ++idx)
            {
                error = ModuleList::GetSharedModule (resolved_exe_file, 
                                                     platform_arch, 
                                                     NULL,
                                                     NULL, 
                                                     0, 
                                                     exe_module_sp, 
                                                     NULL, 
                                                     NULL);
                // Did we find an executable using one of the 
                if (error.Success())
                {
                    if (exe_module_sp && exe_module_sp->GetObjectFile())
                        break;
                    else
                        error.SetErrorToGenericError();
                }
                
                if (idx > 0)
                    arch_names.PutCString (", ");
                arch_names.PutCString (platform_arch.GetArchitectureName());
            }
            
            if (error.Fail() || !exe_module_sp)
            {
                error.SetErrorStringWithFormat ("'%s%s%s' doesn't contain any '%s' platform architectures: %s",
                                                exe_file.GetDirectory().AsCString(""),
                                                exe_file.GetDirectory() ? "/" : "",
                                                exe_file.GetFilename().AsCString(""),
                                                GetShortPluginName(),
                                                arch_names.GetString().c_str());
            }
        }
    }
    else
    {
        error.SetErrorStringWithFormat ("'%s%s%s' does not exist",
                                        exe_file.GetDirectory().AsCString(""),
                                        exe_file.GetDirectory() ? "/" : "",
                                        exe_file.GetFilename().AsCString(""));
    }

    return error;
}


size_t
PlatformDarwin::GetSoftwareBreakpointTrapOpcode (Target &target, BreakpointSite *bp_site)
{
    const uint8_t *trap_opcode = NULL;
    uint32_t trap_opcode_size = 0;
        
    llvm::Triple::ArchType machine = target.GetArchitecture().GetMachine();
    switch (machine)
    {
    case llvm::Triple::x86:
    case llvm::Triple::x86_64:
        {
            static const uint8_t g_i386_breakpoint_opcode[] = { 0xCC };
            trap_opcode = g_i386_breakpoint_opcode;
            trap_opcode_size = sizeof(g_i386_breakpoint_opcode);
        }
        break;
        
    case llvm::Triple::arm:
        {
            static const uint8_t g_arm_breakpoint_opcode[] = { 0xFE, 0xDE, 0xFF, 0xE7 };
            static const uint8_t g_thumb_breakpooint_opcode[] = { 0xFE, 0xDE };

            lldb::BreakpointLocationSP bp_loc_sp (bp_site->GetOwnerAtIndex (0));
            if (bp_loc_sp)
            {
                const AddressClass addr_class = bp_loc_sp->GetAddress().GetAddressClass ();
                if (addr_class == eAddressClassCodeAlternateISA)
                {
                    trap_opcode = g_thumb_breakpooint_opcode;
                    trap_opcode_size = sizeof(g_thumb_breakpooint_opcode);
                    break;
                }
            }
            trap_opcode = g_arm_breakpoint_opcode;
            trap_opcode_size = sizeof(g_arm_breakpoint_opcode);
        }
        break;
        
    case llvm::Triple::ppc:
    case llvm::Triple::ppc64:
        {
            static const uint8_t g_ppc_breakpoint_opcode[] = { 0x7F, 0xC0, 0x00, 0x08 };
            trap_opcode = g_ppc_breakpoint_opcode;
            trap_opcode_size = sizeof(g_ppc_breakpoint_opcode);
        }
        break;
        
    default:
        assert(!"Unhandled architecture in ProcessMacOSX::GetSoftwareBreakpointTrapOpcode()");
        break;
    }
    
    if (trap_opcode && trap_opcode_size)
    {
        if (bp_site->SetTrapOpcode(trap_opcode, trap_opcode_size))
            return trap_opcode_size;
    }
    return 0;

}

bool
PlatformDarwin::GetRemoteOSVersion ()
{
    if (m_remote_platform_sp)
        return m_remote_platform_sp->GetOSVersion (m_major_os_version, 
                                                   m_minor_os_version, 
                                                   m_update_os_version);
    return false;
}

bool
PlatformDarwin::GetRemoteOSBuildString (std::string &s)
{
    if (m_remote_platform_sp)
        return m_remote_platform_sp->GetRemoteOSBuildString (s);
    s.clear();
    return false;
}

bool
PlatformDarwin::GetRemoteOSKernelDescription (std::string &s)
{
    if (m_remote_platform_sp)
        return m_remote_platform_sp->GetRemoteOSKernelDescription (s);
    s.clear();
    return false;
}

// Remote Platform subclasses need to override this function
ArchSpec
PlatformDarwin::GetRemoteSystemArchitecture ()
{
    if (m_remote_platform_sp)
        return m_remote_platform_sp->GetRemoteSystemArchitecture ();
    return ArchSpec();
}


const char *
PlatformDarwin::GetRemoteHostname ()
{
    if (m_remote_platform_sp)
        return m_remote_platform_sp->GetHostname ();
    return NULL;
}

bool
PlatformDarwin::IsConnected () const
{
    if (IsHost())
        return true;
    else if (m_remote_platform_sp)
        return m_remote_platform_sp->IsConnected();
    return false;
}

Error
PlatformDarwin::ConnectRemote (Args& args)
{
    Error error;
    if (IsHost())
    {
        error.SetErrorStringWithFormat ("can't connect to the host platform '%s', always connected", GetShortPluginName());
    }
    else
    {
        if (!m_remote_platform_sp)
            m_remote_platform_sp = Platform::Create ("remote-gdb-server", error);

        if (m_remote_platform_sp)
        {
            if (error.Success())
            {
                if (m_remote_platform_sp)
                {
                    error = m_remote_platform_sp->ConnectRemote (args);
                }
                else
                {
                    error.SetErrorString ("\"platform connect\" takes a single argument: <connect-url>");
                }
            }
        }
        else
            error.SetErrorString ("failed to create a 'remote-gdb-server' platform");
        
        if (error.Fail())
            m_remote_platform_sp.reset();
    }

    return error;
}

Error
PlatformDarwin::DisconnectRemote ()
{
    Error error;
    
    if (IsHost())
    {
        error.SetErrorStringWithFormat ("can't disconnect from the host platform '%s', always connected", GetShortPluginName());
    }
    else
    {
        if (m_remote_platform_sp)
            error = m_remote_platform_sp->DisconnectRemote ();
        else
            error.SetErrorString ("the platform is not currently connected");
    }
    return error;
}

