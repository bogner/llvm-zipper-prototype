//===-- lldb.cpp ------------------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "lldb/lldb-python.h"

#include "lldb/lldb-private.h"
#include "lldb/Core/ArchSpec.h"
#include "lldb/Core/Debugger.h"
#include "lldb/Core/Log.h"
#include "lldb/Core/PluginManager.h"
#include "lldb/Core/RegularExpression.h"
#include "lldb/Core/Timer.h"
#include "lldb/Host/Host.h"
#include "lldb/Host/HostInfo.h"
#include "lldb/Host/Mutex.h"
#include "lldb/Interpreter/ScriptInterpreterPython.h"
#include "lldb/Target/Target.h"
#include "lldb/Target/Thread.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/TargetSelect.h"

#include "Plugins/ABI/MacOSX-i386/ABIMacOSX_i386.h"
#include "Plugins/ABI/MacOSX-arm/ABIMacOSX_arm.h"
#include "Plugins/ABI/MacOSX-arm64/ABIMacOSX_arm64.h"
#include "Plugins/ABI/SysV-x86_64/ABISysV_x86_64.h"
#include "Plugins/ABI/SysV-ppc/ABISysV_ppc.h"
#include "Plugins/ABI/SysV-ppc64/ABISysV_ppc64.h"
#include "Plugins/Disassembler/llvm/DisassemblerLLVMC.h"
#include "Plugins/DynamicLoader/POSIX-DYLD/DynamicLoaderPOSIXDYLD.h"
#include "Plugins/Instruction/ARM/EmulateInstructionARM.h"
#include "Plugins/Instruction/ARM64/EmulateInstructionARM64.h"
#include "Plugins/Instruction/MIPS64/EmulateInstructionMIPS64.h"
#include "Plugins/JITLoader/GDB/JITLoaderGDB.h"
#include "Plugins/LanguageRuntime/CPlusPlus/ItaniumABI/ItaniumABILanguageRuntime.h"
#include "Plugins/ObjectContainer/BSD-Archive/ObjectContainerBSDArchive.h"
#include "Plugins/ObjectFile/ELF/ObjectFileELF.h"
#include "Plugins/ObjectFile/PECOFF/ObjectFilePECOFF.h"
#include "Plugins/Platform/Android/PlatformAndroid.h"
#include "Plugins/Platform/FreeBSD/PlatformFreeBSD.h"
#include "Plugins/Platform/Linux/PlatformLinux.h"
#include "Plugins/Platform/Windows/PlatformWindows.h"
#include "Plugins/Platform/Kalimba/PlatformKalimba.h"
#include "Plugins/Process/elf-core/ProcessElfCore.h"
#include "Plugins/SymbolVendor/ELF/SymbolVendorELF.h"
#include "Plugins/SymbolFile/DWARF/SymbolFileDWARF.h"
#include "Plugins/SymbolFile/DWARF/SymbolFileDWARFDebugMap.h"
#include "Plugins/SymbolFile/Symtab/SymbolFileSymtab.h"
#include "Plugins/UnwindAssembly/x86/UnwindAssembly-x86.h"
#include "Plugins/UnwindAssembly/InstEmulation/UnwindAssemblyInstEmulation.h"

#ifndef LLDB_DISABLE_PYTHON
#include "Plugins/OperatingSystem/Python/OperatingSystemPython.h"
#endif

#include "Plugins/DynamicLoader/MacOSX-DYLD/DynamicLoaderMacOSXDYLD.h"
#include "Plugins/LanguageRuntime/ObjC/AppleObjCRuntime/AppleObjCRuntimeV1.h"
#include "Plugins/LanguageRuntime/ObjC/AppleObjCRuntime/AppleObjCRuntimeV2.h"
#include "Plugins/ObjectContainer/Universal-Mach-O/ObjectContainerUniversalMachO.h"
#include "Plugins/Platform/MacOSX/PlatformMacOSX.h"
#include "Plugins/Platform/MacOSX/PlatformRemoteiOS.h"
#include "Plugins/Platform/MacOSX/PlatformiOSSimulator.h"
#include "Plugins/SystemRuntime/MacOSX/SystemRuntimeMacOSX.h"

#if defined (__APPLE__)
#include "Plugins/DynamicLoader/Darwin-Kernel/DynamicLoaderDarwinKernel.h"
#include "Plugins/ObjectFile/Mach-O/ObjectFileMachO.h"
#include "Plugins/Platform/MacOSX/PlatformDarwinKernel.h"
#include "Plugins/Process/mach-core/ProcessMachCore.h"
#include "Plugins/Process/MacOSX-Kernel/ProcessKDP.h"
#include "Plugins/SymbolVendor/MacOSX/SymbolVendorMacOSX.h"
#endif

#if defined (__linux__)
#include "Plugins/Process/Linux/ProcessLinux.h"
#include "Plugins/Process/POSIX/ProcessPOSIXLog.h"
#endif

#if defined (_WIN32)
#include "lldb/Host/windows/windows.h"
#include "Plugins/Process/Windows/DynamicLoaderWindows.h"
#include "Plugins/Process/Windows/ProcessWindows.h"
#endif

#if defined (__FreeBSD__)
#include "Plugins/Process/POSIX/ProcessPOSIX.h"
#include "Plugins/Process/FreeBSD/ProcessFreeBSD.h"
#endif

#include "Plugins/Platform/gdb-server/PlatformRemoteGDBServer.h"
#include "Plugins/Process/gdb-remote/ProcessGDBRemoteLog.h"
#include "Plugins/Process/gdb-remote/ProcessGDBRemote.h"
#include "Plugins/DynamicLoader/Static/DynamicLoaderStatic.h"
#include "Plugins/MemoryHistory/asan/MemoryHistoryASan.h"
#include "Plugins/InstrumentationRuntime/AddressSanitizer/AddressSanitizerRuntime.h"

using namespace lldb;
using namespace lldb_private;

static void fatal_error_handler(void *user_data, const std::string& reason,
                                bool gen_crash_diag) {
    Host::SetCrashDescription(reason.c_str());
    ::abort();
}

static bool g_inited_for_llgs = false;
void
lldb_private::InitializeForLLGS ()
{
    // Make sure we initialize only once
    static Mutex g_inited_mutex(Mutex::eMutexTypeRecursive);

    Mutex::Locker locker(g_inited_mutex);
    if (!g_inited_for_llgs)
    {
        g_inited_for_llgs = true;

#if defined(_MSC_VER)
        const char *disable_crash_dialog_var = getenv("LLDB_DISABLE_CRASH_DIALOG");
        if (disable_crash_dialog_var && llvm::StringRef(disable_crash_dialog_var).equals_lower("true"))
        {
            // This will prevent Windows from displaying a dialog box requiring user interaction when
            // LLDB crashes.  This is mostly useful when automating LLDB, for example via the test
            // suite, so that a crash in LLDB does not prevent completion of the test suite.
            ::SetErrorMode(GetErrorMode() | SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);

            _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
            _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
            _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
            _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
            _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
            _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
        }
#endif

        Log::Initialize();
        HostInfo::Initialize();
        Timer::Initialize();
        Timer scoped_timer(__PRETTY_FUNCTION__, __PRETTY_FUNCTION__);

        llvm::install_fatal_error_handler(fatal_error_handler, 0);

        ProcessGDBRemoteLog::Initialize();

        // Initialize plug-ins
        ObjectContainerBSDArchive::Initialize();
        ObjectFileELF::Initialize();
        ObjectFilePECOFF::Initialize();
        DynamicLoaderPOSIXDYLD::Initialize();
        PlatformFreeBSD::Initialize();
        PlatformLinux::Initialize();
        PlatformWindows::Initialize();
        PlatformKalimba::Initialize();
        PlatformAndroid::Initialize();

        //----------------------------------------------------------------------
        // Apple/Darwin hosted plugins
        //----------------------------------------------------------------------
        DynamicLoaderMacOSXDYLD::Initialize();
        ObjectContainerUniversalMachO::Initialize();

        PlatformRemoteiOS::Initialize();
        PlatformMacOSX::Initialize();
        PlatformiOSSimulator::Initialize();

#if defined (__APPLE__)
        DynamicLoaderDarwinKernel::Initialize();
        PlatformDarwinKernel::Initialize();
        ObjectFileMachO::Initialize();
#endif
#if defined (__linux__)
        static ConstString g_linux_log_name("linux");
        ProcessPOSIXLog::Initialize(g_linux_log_name);
#endif
#ifndef LLDB_DISABLE_PYTHON
        ScriptInterpreterPython::InitializePrivate();
        OperatingSystemPython::Initialize();
#endif
    }
}

static bool g_inited = false;
void
lldb_private::Initialize ()
{
    // Make sure we initialize only once
    static Mutex g_inited_mutex(Mutex::eMutexTypeRecursive);

    InitializeForLLGS();
    Mutex::Locker locker(g_inited_mutex);
    if (!g_inited)
    {
        g_inited = true;

        // Initialize LLVM and Clang
        llvm::InitializeAllTargets();
        llvm::InitializeAllAsmPrinters();
        llvm::InitializeAllTargetMCs();
        llvm::InitializeAllDisassemblers();

        ABIMacOSX_i386::Initialize();
        ABIMacOSX_arm::Initialize();
        ABIMacOSX_arm64::Initialize();
        ABISysV_x86_64::Initialize();
        ABISysV_ppc::Initialize();
        ABISysV_ppc64::Initialize();
        DisassemblerLLVMC::Initialize();

        JITLoaderGDB::Initialize();
        ProcessElfCore::Initialize();
        MemoryHistoryASan::Initialize();
        AddressSanitizerRuntime::Initialize();

        SymbolVendorELF::Initialize();
        SymbolFileDWARF::Initialize();
        SymbolFileSymtab::Initialize();
        UnwindAssemblyInstEmulation::Initialize();
        UnwindAssembly_x86::Initialize();
        EmulateInstructionARM::Initialize();
        EmulateInstructionARM64::Initialize();
        EmulateInstructionMIPS64::Initialize();
        SymbolFileDWARFDebugMap::Initialize();
        ItaniumABILanguageRuntime::Initialize();
        AppleObjCRuntimeV2::Initialize();
        AppleObjCRuntimeV1::Initialize();
        SystemRuntimeMacOSX::Initialize();

#if defined (__linux__)
        //----------------------------------------------------------------------
        // Linux hosted plugins
        //----------------------------------------------------------------------
        ProcessLinux::Initialize();
#endif
#if defined(_MSC_VER)
        DynamicLoaderWindows::Initialize();
        ProcessWindows::Initialize();
#endif
#if defined (__FreeBSD__)
        ProcessFreeBSD::Initialize();
#endif
#if defined (__APPLE__)
        SymbolVendorMacOSX::Initialize();
        ProcessKDP::Initialize();
        ProcessMachCore::Initialize();
#endif
        //----------------------------------------------------------------------
        // Platform agnostic plugins
        //----------------------------------------------------------------------
        PlatformRemoteGDBServer::Initialize();

        ProcessGDBRemote::Initialize();
        DynamicLoaderStatic::Initialize();

        // Scan for any system or user LLDB plug-ins
        PluginManager::Initialize();

        // The process settings need to know about installed plug-ins, so the Settings must be initialized
        // AFTER PluginManager::Initialize is called.

        Debugger::SettingsInitialize();

    }
}

void
lldb_private::WillTerminate()
{
    Host::WillTerminate();
}

void
lldb_private::TerminateLLGS ()
{
    if (g_inited_for_llgs)
    {
        g_inited_for_llgs = false;

        Timer scoped_timer (__PRETTY_FUNCTION__, __PRETTY_FUNCTION__);
        ObjectContainerBSDArchive::Terminate();
        ObjectFileELF::Terminate();
        ObjectFilePECOFF::Terminate ();
        DynamicLoaderPOSIXDYLD::Terminate ();
        PlatformFreeBSD::Terminate();
        PlatformLinux::Terminate();
        PlatformWindows::Terminate();
        PlatformKalimba::Terminate();
        PlatformAndroid::Terminate();
        DynamicLoaderMacOSXDYLD::Terminate();
        ObjectContainerUniversalMachO::Terminate();
        PlatformMacOSX::Terminate();
        PlatformRemoteiOS::Terminate();
        PlatformiOSSimulator::Terminate();

#if defined (__APPLE__)
        DynamicLoaderDarwinKernel::Terminate();
        ObjectFileMachO::Terminate();
        PlatformDarwinKernel::Terminate();
#endif

#ifndef LLDB_DISABLE_PYTHON
        OperatingSystemPython::Terminate();
#endif

        Log::Terminate();
    }
}

void
lldb_private::Terminate ()
{
    if (g_inited)
    {
        g_inited = false;

        Timer scoped_timer (__PRETTY_FUNCTION__, __PRETTY_FUNCTION__);
        // Terminate and unload and loaded system or user LLDB plug-ins
        PluginManager::Terminate();
        ABIMacOSX_i386::Terminate();
        ABIMacOSX_arm::Terminate();
        ABIMacOSX_arm64::Terminate();
        ABISysV_x86_64::Terminate();
        ABISysV_ppc::Terminate();
        ABISysV_ppc64::Terminate();
        DisassemblerLLVMC::Terminate();

        JITLoaderGDB::Terminate();
        ProcessElfCore::Terminate();
        MemoryHistoryASan::Terminate();
        AddressSanitizerRuntime::Terminate();
        SymbolVendorELF::Terminate();
        SymbolFileDWARF::Terminate();
        SymbolFileSymtab::Terminate();
        UnwindAssembly_x86::Terminate();
        UnwindAssemblyInstEmulation::Terminate();
        EmulateInstructionARM::Terminate();
        EmulateInstructionARM64::Terminate();
        EmulateInstructionMIPS64::Terminate();
        SymbolFileDWARFDebugMap::Terminate();
        ItaniumABILanguageRuntime::Terminate();
        AppleObjCRuntimeV2::Terminate();
        AppleObjCRuntimeV1::Terminate();
        SystemRuntimeMacOSX::Terminate();

#if defined (__APPLE__)
        ProcessMachCore::Terminate();
        ProcessKDP::Terminate();
        SymbolVendorMacOSX::Terminate();
#endif
#if defined(_MSC_VER)
        DynamicLoaderWindows::Terminate();
#endif

#if defined (__linux__)
        ProcessLinux::Terminate();
#endif

#if defined (__FreeBSD__)
        ProcessFreeBSD::Terminate();
#endif
        Debugger::SettingsTerminate ();

        PlatformRemoteGDBServer::Terminate();
        ProcessGDBRemote::Terminate();
        DynamicLoaderStatic::Terminate();

        TerminateLLGS();
    }
}

#if defined (__APPLE__)
extern "C" const unsigned char liblldb_coreVersionString[];
#else

#include "clang/Basic/Version.h"

static const char *
GetLLDBRevision()
{
#ifdef LLDB_REVISION
    return LLDB_REVISION;
#else
    return NULL;
#endif
}

static const char *
GetLLDBRepository()
{
#ifdef LLDB_REPOSITORY
    return LLDB_REPOSITORY;
#else
    return NULL;
#endif
}

#endif

const char *
lldb_private::GetVersion ()
{
#if defined (__APPLE__)
    static char g_version_string[32];
    if (g_version_string[0] == '\0')
    {
        const char *version_string = ::strstr ((const char *)liblldb_coreVersionString, "PROJECT:");
        
        if (version_string)
            version_string += sizeof("PROJECT:") - 1;
        else
            version_string = "unknown";
        
        const char *newline_loc = strchr(version_string, '\n');
        
        size_t version_len = sizeof(g_version_string) - 1;
        
        if (newline_loc &&
            (newline_loc - version_string < static_cast<ptrdiff_t>(version_len)))
            version_len = newline_loc - version_string;
        
        ::snprintf(g_version_string, version_len + 1, "%s", version_string);
    }

    return g_version_string;
#else
    // On Linux/FreeBSD/Windows, report a version number in the same style as the clang tool.
    static std::string g_version_str;
    if (g_version_str.empty())
    {
        g_version_str += "lldb version ";
        g_version_str += CLANG_VERSION_STRING;
        const char * lldb_repo = GetLLDBRepository();
        if (lldb_repo)
        {
            g_version_str += " (";
            g_version_str += lldb_repo;
        }

        const char *lldb_rev = GetLLDBRevision();
        if (lldb_rev)
        {
            g_version_str += " revision ";
            g_version_str += lldb_rev;
        }
        std::string clang_rev (clang::getClangRevision());
        if (clang_rev.length() > 0)
        {
            g_version_str += " clang revision ";
            g_version_str += clang_rev;
        }
        std::string llvm_rev (clang::getLLVMRevision());
        if (llvm_rev.length() > 0)
        {
            g_version_str += " llvm revision ";
            g_version_str += llvm_rev;
        }

        if (lldb_repo)
            g_version_str += ")";
    }
    return g_version_str.c_str();
#endif
}
