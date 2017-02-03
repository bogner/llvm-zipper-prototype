//===-- PlatformFreeBSD.cpp -------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "PlatformFreeBSD.h"
#include "lldb/Host/Config.h"

// C Includes
#include <stdio.h>
#ifndef LLDB_DISABLE_POSIX
#include <sys/utsname.h>
#endif

// C++ Includes
// Other libraries and framework includes
// Project includes
#include "lldb/Breakpoint/BreakpointLocation.h"
#include "lldb/Breakpoint/BreakpointSite.h"
#include "lldb/Core/Debugger.h"
#include "lldb/Core/Module.h"
#include "lldb/Core/ModuleSpec.h"
#include "lldb/Core/PluginManager.h"
#include "lldb/Host/Host.h"
#include "lldb/Host/HostInfo.h"
#include "lldb/Target/Process.h"
#include "lldb/Utility/Error.h"

using namespace lldb;
using namespace lldb_private;
using namespace lldb_private::platform_freebsd;

PlatformSP PlatformFreeBSD::CreateInstance(bool force, const ArchSpec *arch) {
  // The only time we create an instance is when we are creating a remote
  // freebsd platform
  const bool is_host = false;

  bool create = force;
  if (create == false && arch && arch->IsValid()) {
    const llvm::Triple &triple = arch->GetTriple();
    switch (triple.getOS()) {
    case llvm::Triple::FreeBSD:
      create = true;
      break;

#if defined(__FreeBSD__) || defined(__OpenBSD__)
    // Only accept "unknown" for the OS if the host is BSD and
    // it "unknown" wasn't specified (it was just returned because it
    // was NOT specified)
    case llvm::Triple::OSType::UnknownOS:
      create = !arch->TripleOSWasSpecified();
      break;
#endif
    default:
      break;
    }
  }
  if (create)
    return PlatformSP(new PlatformFreeBSD(is_host));
  return PlatformSP();
}

ConstString PlatformFreeBSD::GetPluginNameStatic(bool is_host) {
  if (is_host) {
    static ConstString g_host_name(Platform::GetHostPlatformName());
    return g_host_name;
  } else {
    static ConstString g_remote_name("remote-freebsd");
    return g_remote_name;
  }
}

const char *PlatformFreeBSD::GetDescriptionStatic(bool is_host) {
  if (is_host)
    return "Local FreeBSD user platform plug-in.";
  else
    return "Remote FreeBSD user platform plug-in.";
}

static uint32_t g_initialize_count = 0;

void PlatformFreeBSD::Initialize() {
  Platform::Initialize();

  if (g_initialize_count++ == 0) {
#if defined(__FreeBSD__)
    // Force a host flag to true for the default platform object.
    PlatformSP default_platform_sp(new PlatformFreeBSD(true));
    default_platform_sp->SetSystemArchitecture(HostInfo::GetArchitecture());
    Platform::SetHostPlatform(default_platform_sp);
#endif
    PluginManager::RegisterPlugin(PlatformFreeBSD::GetPluginNameStatic(false),
                                  PlatformFreeBSD::GetDescriptionStatic(false),
                                  PlatformFreeBSD::CreateInstance);
  }
}

void PlatformFreeBSD::Terminate() {
  if (g_initialize_count > 0 && --g_initialize_count == 0)
    PluginManager::UnregisterPlugin(PlatformFreeBSD::CreateInstance);

  Platform::Terminate();
}

bool PlatformFreeBSD::GetModuleSpec(const FileSpec &module_file_spec,
                                    const ArchSpec &arch,
                                    ModuleSpec &module_spec) {
  if (m_remote_platform_sp)
    return m_remote_platform_sp->GetModuleSpec(module_file_spec, arch,
                                               module_spec);

  return Platform::GetModuleSpec(module_file_spec, arch, module_spec);
}

Error PlatformFreeBSD::RunShellCommand(const char *command,
                                       const FileSpec &working_dir,
                                       int *status_ptr, int *signo_ptr,
                                       std::string *command_output,
                                       uint32_t timeout_sec) {
  if (IsHost())
    return Host::RunShellCommand(command, working_dir, status_ptr, signo_ptr,
                                 command_output, timeout_sec);
  else {
    if (m_remote_platform_sp)
      return m_remote_platform_sp->RunShellCommand(command, working_dir,
                                                   status_ptr, signo_ptr,
                                                   command_output, timeout_sec);
    else
      return Error("unable to run a remote command without a platform");
  }
}

//------------------------------------------------------------------
/// Default Constructor
//------------------------------------------------------------------
PlatformFreeBSD::PlatformFreeBSD(bool is_host)
    : Platform(is_host), m_remote_platform_sp() {}

//------------------------------------------------------------------
/// Destructor.
///
/// The destructor is virtual since this class is designed to be
/// inherited from by the plug-in instance.
//------------------------------------------------------------------
PlatformFreeBSD::~PlatformFreeBSD() {}

// TODO:VK: inherit PlatformPOSIX

bool PlatformFreeBSD::GetRemoteOSVersion() {
  if (m_remote_platform_sp)
    return m_remote_platform_sp->GetOSVersion(
        m_major_os_version, m_minor_os_version, m_update_os_version);
  return false;
}

bool PlatformFreeBSD::GetRemoteOSBuildString(std::string &s) {
  if (m_remote_platform_sp)
    return m_remote_platform_sp->GetRemoteOSBuildString(s);
  s.clear();
  return false;
}

bool PlatformFreeBSD::GetRemoteOSKernelDescription(std::string &s) {
  if (m_remote_platform_sp)
    return m_remote_platform_sp->GetRemoteOSKernelDescription(s);
  s.clear();
  return false;
}

// Remote Platform subclasses need to override this function
ArchSpec PlatformFreeBSD::GetRemoteSystemArchitecture() {
  if (m_remote_platform_sp)
    return m_remote_platform_sp->GetRemoteSystemArchitecture();
  return ArchSpec();
}

const char *PlatformFreeBSD::GetHostname() {
  if (IsHost())
    return Platform::GetHostname();

  if (m_remote_platform_sp)
    return m_remote_platform_sp->GetHostname();
  return NULL;
}

bool PlatformFreeBSD::IsConnected() const {
  if (IsHost())
    return true;
  else if (m_remote_platform_sp)
    return m_remote_platform_sp->IsConnected();
  return false;
}

Error PlatformFreeBSD::ConnectRemote(Args &args) {
  Error error;
  if (IsHost()) {
    error.SetErrorStringWithFormat(
        "can't connect to the host platform '%s', always connected",
        GetPluginName().GetCString());
  } else {
    if (!m_remote_platform_sp)
      m_remote_platform_sp =
          Platform::Create(ConstString("remote-gdb-server"), error);

    if (m_remote_platform_sp) {
      if (error.Success()) {
        if (m_remote_platform_sp) {
          error = m_remote_platform_sp->ConnectRemote(args);
        } else {
          error.SetErrorString(
              "\"platform connect\" takes a single argument: <connect-url>");
        }
      }
    } else
      error.SetErrorString("failed to create a 'remote-gdb-server' platform");

    if (error.Fail())
      m_remote_platform_sp.reset();
  }

  return error;
}

Error PlatformFreeBSD::DisconnectRemote() {
  Error error;

  if (IsHost()) {
    error.SetErrorStringWithFormat(
        "can't disconnect from the host platform '%s', always connected",
        GetPluginName().GetCString());
  } else {
    if (m_remote_platform_sp)
      error = m_remote_platform_sp->DisconnectRemote();
    else
      error.SetErrorString("the platform is not currently connected");
  }
  return error;
}

const char *PlatformFreeBSD::GetUserName(uint32_t uid) {
  // Check the cache in Platform in case we have already looked this uid up
  const char *user_name = Platform::GetUserName(uid);
  if (user_name)
    return user_name;

  if (IsRemote() && m_remote_platform_sp)
    return m_remote_platform_sp->GetUserName(uid);
  return NULL;
}

const char *PlatformFreeBSD::GetGroupName(uint32_t gid) {
  const char *group_name = Platform::GetGroupName(gid);
  if (group_name)
    return group_name;

  if (IsRemote() && m_remote_platform_sp)
    return m_remote_platform_sp->GetGroupName(gid);
  return NULL;
}

Error PlatformFreeBSD::GetSharedModule(
    const ModuleSpec &module_spec, Process *process, ModuleSP &module_sp,
    const FileSpecList *module_search_paths_ptr, ModuleSP *old_module_sp_ptr,
    bool *did_create_ptr) {
  Error error;
  module_sp.reset();

  if (IsRemote()) {
    // If we have a remote platform always, let it try and locate
    // the shared module first.
    if (m_remote_platform_sp) {
      error = m_remote_platform_sp->GetSharedModule(
          module_spec, process, module_sp, module_search_paths_ptr,
          old_module_sp_ptr, did_create_ptr);
    }
  }

  if (!module_sp) {
    // Fall back to the local platform and find the file locally
    error = Platform::GetSharedModule(module_spec, process, module_sp,
                                      module_search_paths_ptr,
                                      old_module_sp_ptr, did_create_ptr);
  }
  if (module_sp)
    module_sp->SetPlatformFileSpec(module_spec.GetFileSpec());
  return error;
}

bool PlatformFreeBSD::GetSupportedArchitectureAtIndex(uint32_t idx,
                                                      ArchSpec &arch) {
  if (IsHost()) {
    ArchSpec hostArch = HostInfo::GetArchitecture(HostInfo::eArchKindDefault);
    if (hostArch.GetTriple().isOSFreeBSD()) {
      if (idx == 0) {
        arch = hostArch;
        return arch.IsValid();
      } else if (idx == 1) {
        // If the default host architecture is 64-bit, look for a 32-bit variant
        if (hostArch.IsValid() && hostArch.GetTriple().isArch64Bit()) {
          arch = HostInfo::GetArchitecture(HostInfo::eArchKind32);
          return arch.IsValid();
        }
      }
    }
  } else {
    if (m_remote_platform_sp)
      return m_remote_platform_sp->GetSupportedArchitectureAtIndex(idx, arch);

    llvm::Triple triple;
    // Set the OS to FreeBSD
    triple.setOS(llvm::Triple::FreeBSD);
    // Set the architecture
    switch (idx) {
    case 0:
      triple.setArchName("x86_64");
      break;
    case 1:
      triple.setArchName("i386");
      break;
    case 2:
      triple.setArchName("aarch64");
      break;
    case 3:
      triple.setArchName("arm");
      break;
    case 4:
      triple.setArchName("mips64");
      break;
    case 5:
      triple.setArchName("mips");
      break;
    case 6:
      triple.setArchName("ppc64");
      break;
    case 7:
      triple.setArchName("ppc");
      break;
    default:
      return false;
    }
    // Leave the vendor as "llvm::Triple:UnknownVendor" and don't specify the
    // vendor by
    // calling triple.SetVendorName("unknown") so that it is a "unspecified
    // unknown".
    // This means when someone calls triple.GetVendorName() it will return an
    // empty string
    // which indicates that the vendor can be set when two architectures are
    // merged

    // Now set the triple into "arch" and return true
    arch.SetTriple(triple);
    return true;
  }
  return false;
}

void PlatformFreeBSD::GetStatus(Stream &strm) {
#ifndef LLDB_DISABLE_POSIX
  struct utsname un;

  strm << "      Host: ";

  ::memset(&un, 0, sizeof(utsname));
  if (uname(&un) == -1)
    strm << "FreeBSD" << '\n';

  strm << un.sysname << ' ' << un.release;
  if (un.nodename[0] != '\0')
    strm << " (" << un.nodename << ')';
  strm << '\n';

  // Dump a common information about the platform status.
  strm << "Host: " << un.sysname << ' ' << un.release << ' ' << un.version
       << '\n';
#endif

  Platform::GetStatus(strm);
}

size_t
PlatformFreeBSD::GetSoftwareBreakpointTrapOpcode(Target &target,
                                                 BreakpointSite *bp_site) {
  switch (target.GetArchitecture().GetMachine()) {
  case llvm::Triple::arm: {
    lldb::BreakpointLocationSP bp_loc_sp(bp_site->GetOwnerAtIndex(0));
    AddressClass addr_class = eAddressClassUnknown;

    if (bp_loc_sp) {
      addr_class = bp_loc_sp->GetAddress().GetAddressClass();
      if (addr_class == eAddressClassUnknown &&
          (bp_loc_sp->GetAddress().GetFileAddress() & 1))
        addr_class = eAddressClassCodeAlternateISA;
    }

    if (addr_class == eAddressClassCodeAlternateISA) {
      // TODO: Enable when FreeBSD supports thumb breakpoints.
      // FreeBSD kernel as of 10.x, does not support thumb breakpoints
      return 0;
    }

    static const uint8_t g_arm_breakpoint_opcode[] = {0xFE, 0xDE, 0xFF, 0xE7};
    size_t trap_opcode_size = sizeof(g_arm_breakpoint_opcode);
    assert(bp_site);
    if (bp_site->SetTrapOpcode(g_arm_breakpoint_opcode, trap_opcode_size))
      return trap_opcode_size;
  }
    LLVM_FALLTHROUGH;
  default:
    return Platform::GetSoftwareBreakpointTrapOpcode(target, bp_site);
  }
}

void PlatformFreeBSD::CalculateTrapHandlerSymbolNames() {
  m_trap_handlers.push_back(ConstString("_sigtramp"));
}

Error PlatformFreeBSD::LaunchProcess(ProcessLaunchInfo &launch_info) {
  Error error;
  if (IsHost()) {
    error = Platform::LaunchProcess(launch_info);
  } else {
    if (m_remote_platform_sp)
      error = m_remote_platform_sp->LaunchProcess(launch_info);
    else
      error.SetErrorString("the platform is not currently connected");
  }
  return error;
}

lldb::ProcessSP PlatformFreeBSD::Attach(ProcessAttachInfo &attach_info,
                                        Debugger &debugger, Target *target,
                                        Error &error) {
  lldb::ProcessSP process_sp;
  if (IsHost()) {
    if (target == NULL) {
      TargetSP new_target_sp;
      ArchSpec emptyArchSpec;

      error = debugger.GetTargetList().CreateTarget(debugger, "", emptyArchSpec,
                                                    false, m_remote_platform_sp,
                                                    new_target_sp);
      target = new_target_sp.get();
    } else
      error.Clear();

    if (target && error.Success()) {
      debugger.GetTargetList().SetSelectedTarget(target);
      // The freebsd always currently uses the GDB remote debugger plug-in
      // so even when debugging locally we are debugging remotely!
      // Just like the darwin plugin.
      process_sp = target->CreateProcess(
          attach_info.GetListenerForProcess(debugger), "gdb-remote", NULL);

      if (process_sp)
        error = process_sp->Attach(attach_info);
    }
  } else {
    if (m_remote_platform_sp)
      process_sp =
          m_remote_platform_sp->Attach(attach_info, debugger, target, error);
    else
      error.SetErrorString("the platform is not currently connected");
  }
  return process_sp;
}
