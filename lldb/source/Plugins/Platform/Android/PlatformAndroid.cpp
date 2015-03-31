//===-- PlatformAndroid.cpp -------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

// C Includes
// C++ Includes
// Other libraries and framework includes
#include "lldb/Core/Log.h"
#include "lldb/Core/PluginManager.h"
#include "lldb/Host/HostInfo.h"

// Project includes
#include "AdbClient.h"
#include "PlatformAndroid.h"
#include "PlatformAndroidRemoteGDBServer.h"

using namespace lldb;
using namespace lldb_private;
using namespace lldb_private::platform_android;

static uint32_t g_initialize_count = 0;

void
PlatformAndroid::Initialize ()
{
    PlatformLinux::Initialize ();

    if (g_initialize_count++ == 0)
    {
#if defined(__ANDROID__)
        PlatformSP default_platform_sp (new PlatformAndroid(true));
        default_platform_sp->SetSystemArchitecture(HostInfo::GetArchitecture());
        Platform::SetHostPlatform (default_platform_sp);
#endif
        PluginManager::RegisterPlugin (PlatformAndroid::GetPluginNameStatic(false),
                                       PlatformAndroid::GetPluginDescriptionStatic(false),
                                       PlatformAndroid::CreateInstance);
    }
}

void
PlatformAndroid::Terminate ()
{
    if (g_initialize_count > 0)
    {
        if (--g_initialize_count == 0)
        {
            PluginManager::UnregisterPlugin (PlatformAndroid::CreateInstance);
        }
    }

    PlatformLinux::Terminate ();
}

PlatformSP
PlatformAndroid::CreateInstance (bool force, const ArchSpec *arch)
{
    Log *log(GetLogIfAllCategoriesSet (LIBLLDB_LOG_PLATFORM));
    if (log)
    {
        const char *arch_name;
        if (arch && arch->GetArchitectureName ())
            arch_name = arch->GetArchitectureName ();
        else
            arch_name = "<null>";

        const char *triple_cstr = arch ? arch->GetTriple ().getTriple ().c_str() : "<null>";

        log->Printf ("PlatformAndroid::%s(force=%s, arch={%s,%s})", __FUNCTION__, force ? "true" : "false", arch_name, triple_cstr);
    }

    bool create = force;
    if (create == false && arch && arch->IsValid())
    {
        const llvm::Triple &triple = arch->GetTriple();
        switch (triple.getVendor())
        {
            case llvm::Triple::PC:
                create = true;
                break;

#if defined(__ANDROID__)
            // Only accept "unknown" for the vendor if the host is android and
            // it "unknown" wasn't specified (it was just returned because it
            // was NOT specified_
            case llvm::Triple::VendorType::UnknownVendor:
                create = !arch->TripleVendorWasSpecified();
                break;
#endif
            default:
                break;
        }
        
        if (create)
        {
            switch (triple.getOS())
            {
                case llvm::Triple::Android:
                    break;

#if defined(__ANDROID__)
                // Only accept "unknown" for the OS if the host is android and
                // it "unknown" wasn't specified (it was just returned because it
                // was NOT specified)
                case llvm::Triple::OSType::UnknownOS:
                    create = !arch->TripleOSWasSpecified();
                    break;
#endif
                default:
                    create = false;
                    break;
            }
        }
    }

    if (create)
    {
        if (log)
            log->Printf ("PlatformAndroid::%s() creating remote-android platform", __FUNCTION__);
        return PlatformSP(new PlatformAndroid(false));
    }

    if (log)
        log->Printf ("PlatformAndroid::%s() aborting creation of remote-android platform", __FUNCTION__);

    return PlatformSP();
}

PlatformAndroid::PlatformAndroid (bool is_host) :
    PlatformLinux(is_host)
{
}

PlatformAndroid::~PlatformAndroid()
{
}

ConstString
PlatformAndroid::GetPluginNameStatic (bool is_host)
{
    if (is_host)
    {
        static ConstString g_host_name(Platform::GetHostPlatformName ());
        return g_host_name;
    }
    else
    {
        static ConstString g_remote_name("remote-android");
        return g_remote_name;
    }
}

const char *
PlatformAndroid::GetPluginDescriptionStatic (bool is_host)
{
    if (is_host)
        return "Local Android user platform plug-in.";
    else
        return "Remote Android user platform plug-in.";
}

ConstString
PlatformAndroid::GetPluginName()
{
    return GetPluginNameStatic(IsHost());
}

Error
PlatformAndroid::ConnectRemote (Args& args)
{
    m_device_id.clear ();

    if (IsHost())
    {
        return Error ("can't connect to the host platform '%s', always connected", GetPluginName().GetCString());
    }

    if (!m_remote_platform_sp)
        m_remote_platform_sp = PlatformSP(new PlatformAndroidRemoteGDBServer());

    auto error = PlatformLinux::ConnectRemote (args);
    if (error.Success ())
    {
        // Fetch the device list from ADB and if only 1 device found then use that device
        // TODO: Handle the case when more device is available
        AdbClient adb;
        error = AdbClient::CreateByDeviceID (nullptr, adb);
        if (error.Fail ())
            return error;

        m_device_id = adb.GetDeviceID ();
    }
    return error;
}

const char *
PlatformAndroid::GetCacheHostname ()
{
    return m_device_id.c_str ();
}
